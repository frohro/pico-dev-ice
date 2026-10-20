#include "openhpsdr.h"
#include <string.h>
#include "pico/cyw43_arch.h"
#include "pico/bootrom.h"
#include "cyw43_internal.h"
#include "lwip/pbuf.h"

static struct udp_pcb *s_pcb = NULL;
static ip_addr_t s_host_ip;
static u16_t s_host_port = 0;
static bool s_active = false;
static uint32_t s_sequence = 0;

static hpsdr_freq_callback_t s_freq_cb = NULL;
static hpsdr_rate_callback_t s_rate_cb = NULL;
static hpsdr_gain_callback_t s_gain_cb = NULL;

static uint8_t s_packet_buffer[HPSDR_PACKET_SIZE];
static uint32_t s_sample_idx = 0; // 0 to 125 samples (63 in subframe 1, 63 in subframe 2)

static uint32_t s_push_calls = 0;
static uint32_t s_pkts_sent = 0;
static uint32_t s_pbuf_alloc_failed = 0;
static uint32_t s_udp_err = 0;
static uint32_t s_max_send_us = 0;
static uint32_t s_last_send_us = 0;
static uint32_t s_stall_seq = 0;
static uint32_t s_stall_dt = 0;

void openhpsdr_get_stats(uint32_t *push_calls, uint32_t *pkts_sent, uint32_t *pbuf_failed, uint32_t *udp_err, uint32_t *max_us, uint32_t *last_us, uint32_t *stall_seq, uint32_t *stall_dt) {
    if (push_calls) *push_calls = s_push_calls;
    if (pkts_sent) *pkts_sent = s_pkts_sent;
    if (pbuf_failed) *pbuf_failed = s_pbuf_alloc_failed;
    if (udp_err) *udp_err = s_udp_err;
    if (max_us) *max_us = s_max_send_us;
    if (last_us) *last_us = s_last_send_us;
    if (stall_seq) *stall_seq = s_stall_seq;
    if (stall_dt) *stall_dt = s_stall_dt;
}

struct udp_pcb *openhpsdr_get_pcb(void) {
    return s_pcb;
}

static void send_discovery_reply(const ip_addr_t *addr, u16_t port) {
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 60, PBUF_POOL);
    if (p) {
        uint8_t *payload = (uint8_t *)p->payload;
        memset(payload, 0, 60);

        payload[0] = 0xEF;
        payload[1] = 0xFE;
        payload[2] = 0x02; // Discovery response

        // Get Pico W MAC address
        uint8_t mac[6] = {0};
        cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
        memcpy(&payload[3], mac, 6);

        payload[9]  = 0x21; // Firmware Version: 3.3
        payload[10] = 0x01; // Board ID: 0x01 = Hermes (compatible with SDR++, Quisk, PowerSDR, Thetis)
        payload[11] = 0x01; // Protocol Version: 1 (EP6 / EP2 UDP)
        payload[12] = 0x01; // Number of DDC Receivers: 1
        payload[13] = 0x01; // Number of ADCs: 1

        udp_sendto(s_pcb, p, addr, port);
        pbuf_free(p);
    }
}

static uint32_t s_last_parsed_freq = 0;
static int s_last_gain_raw = -1;       // last 0x0A gain value seen (-1 = none this session)
static bool s_seen_rx1_freq = false;   // host uses RX1 NCO (addr 0x02); then ignore TX NCO (0x01)

// Called whenever a new streaming session begins.
static void session_reset(void) {
    s_sequence = 0;
    s_sample_idx = 0;
    s_last_gain_raw = -1;       // let the host's initial gain command apply again
    s_seen_rx1_freq = false;
}

static void handle_cc_packet(const uint8_t *data, uint16_t len) {
    if (len < 8) return;

    uint32_t target_freq = 0;

    // Check for C&C sync pattern 0x7F 0x7F 0x7F across subframes
    for (int offset = 8; offset + 7 < (int)len; offset += 512) {
        if (data[offset] == 0x7F && data[offset+1] == 0x7F && data[offset+2] == 0x7F) {
            uint8_t c0 = data[offset + 3];
            uint8_t c1 = data[offset + 4];
            uint8_t c2 = data[offset + 5];
            uint8_t c3 = data[offset + 6];
            uint8_t c4 = data[offset + 7];

            uint8_t command_type = (c0 >> 1) & 0x3F;

            // Address 0x02 = RX1 NCO, address 0x01 = TX NCO. This radio has one DDC, so
            // RX1 is authoritative. SDR++ (Hermes-Lite mode) only ever sends 0x01, so
            // 0x01 is used when the host never sends 0x02. Hosts that send both
            // (Thetis, ...) would otherwise make the FPGA retune back and forth.
            // Addresses 0x03..0x08 (RX2..RX7) and 0x09 (drive/filter bits, NOT a
            // frequency) are ignored.
            if (command_type == 0x01 || command_type == 0x02) {
                uint32_t freq_hz = ((uint32_t)c1 << 24) | ((uint32_t)c2 << 16) | ((uint32_t)c3 << 8) | c4;
                if (freq_hz <= 30000000) {
                    if (command_type == 0x02) {
                        s_seen_rx1_freq = true;
                        target_freq = freq_hz;
                    } else if (!s_seen_rx1_freq) {
                        target_freq = freq_hz;
                    }
                }
            }
            // Command 0x0A: Hermes-Lite RX LNA Gain (from SDR++ gain slider: 0..60 dB)
            else if (command_type == 0x0A) {
                uint8_t raw_gain = c4 & 0x3F; // 0 to 60 dB
                // Hosts re-send this command periodically. Only act when the slider
                // value CHANGES, otherwise it would keep undoing the overload AGC.
                if ((int)raw_gain != s_last_gain_raw) {
                    s_last_gain_raw = raw_gain;
                    // Map 0..60 dB slider to 4-bit PGA code (0..15, where 0=max gain, 15=min gain):
                    uint8_t pga_code = (raw_gain >= 60) ? 0 : (uint8_t)(15 - ((uint32_t)raw_gain * 15 / 60));
                    if (s_gain_cb) {
                        s_gain_cb(pga_code);
                    }
                }
            }
            // Command 0x00: General control (sample rate & preamp)
            else if (command_type == 0x00) {
                uint8_t speed = c1 & 0x03;
                uint32_t rate = (speed == 0x01) ? 96000 : 48000;
                if (s_rate_cb) {
                    s_rate_cb(rate);
                }
                // Gain is intentionally NOT touched here: it changes only via
                // the FPGA overload (OTR) AGC or the explicit 0x0A gain command.
            }
        }
    }

    if (target_freq > 0 && target_freq != s_last_parsed_freq && s_freq_cb) {
        s_last_parsed_freq = target_freq;
        s_freq_cb(target_freq);
    }
}

static volatile bool s_no_watchdog = false;
static uint32_t s_last_packet_rx_ms = 0;

static void hpsdr_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb;
    if (!p) return;

    s_last_packet_rx_ms = to_ms_since_boot(get_absolute_time());

    uint8_t *data = (uint8_t *)p->payload;

    // Discovery Broadcast: 0xEFFE 0x02
    if (p->len >= 3 && data[0] == 0xEF && data[1] == 0xFE && data[2] == 0x02) {
        send_discovery_reply(addr, port);
    }
    // Start / Stop / Run command: 0xEFFE 0x04
    else if (p->len >= 4 && data[0] == 0xEF && data[1] == 0xFE && data[2] == 0x04) {
        if (data[3] & 0x01) {
            if (!s_active) {
                session_reset();
            }
            s_active = true;
            s_no_watchdog = (data[3] & 0x80) != 0;
            ip_addr_copy(s_host_ip, *addr);
            s_host_port = port;
        } else {
            s_active = false;
            s_no_watchdog = false;
            s_host_port = 0;
            s_sequence = 0;
            s_sample_idx = 0;
        }
    }
    // Remote BOOTSEL reboot: 0xEFFE 0xBB
    else if (p->len >= 3 && data[0] == 0xEF && data[1] == 0xFE && data[2] == 0xBB) {
        pbuf_free(p);
        reset_usb_boot(0, 0);
        return;
    }
    // Remote stats query: 0xEFFE 0xCC (8 words, original layout)
    else if (p->len >= 3 && data[0] == 0xEF && data[1] == 0xFE && data[2] == 0xCC) {
        struct pbuf *rp = pbuf_alloc(PBUF_TRANSPORT, 32, PBUF_POOL);
        if (rp) {
            uint32_t stats[8];
            stats[0] = s_push_calls;
            stats[1] = s_pkts_sent;
            stats[2] = s_udp_err;
            stats[3] = s_pbuf_alloc_failed;
            stats[4] = s_max_send_us;
            stats[5] = s_last_send_us;
            stats[6] = get_ring_overruns();
            stats[7] = get_dma_irq_count();
            pbuf_take(rp, stats, sizeof(stats));
            udp_sendto(s_pcb, rp, addr, port);
            pbuf_free(rp);
        }
    }
    // Remote extended stats query: 0xEFFE 0xCD (12 words: the 8 above, then
    // PIO RX stalls, ring fill now, ring max fill, last >100 ms send stall seq)
    else if (p->len >= 3 && data[0] == 0xEF && data[1] == 0xFE && data[2] == 0xCD) {
        struct pbuf *rp = pbuf_alloc(PBUF_TRANSPORT, 48, PBUF_POOL);
        if (rp) {
            uint32_t stats[12];
            stats[0] = s_push_calls;
            stats[1] = s_pkts_sent;
            stats[2] = s_udp_err;
            stats[3] = s_pbuf_alloc_failed;
            stats[4] = s_max_send_us;
            stats[5] = s_last_send_us;
            stats[6] = get_ring_overruns();
            stats[7] = get_dma_irq_count();
            stats[8] = get_pio_rx_stalls();
            stats[9] = get_ring_fill();
            stats[10] = get_ring_max_fill();
            stats[11] = s_stall_seq;
            pbuf_take(rp, stats, sizeof(stats));
            udp_sendto(s_pcb, rp, addr, port);
            pbuf_free(rp);
        }
    }
    // Command & Control (C&C) or standard data: 0xEFFE 0x01
    else if (p->len >= 8 && data[0] == 0xEF && data[1] == 0xFE) {
        if (!s_active) {
            session_reset();
        }
        ip_addr_copy(s_host_ip, *addr);
        s_host_port = port;
        s_active = true;
        handle_cc_packet(data, p->len);
    }
    pbuf_free(p);
}

void openhpsdr_init(hpsdr_freq_callback_t on_freq, hpsdr_rate_callback_t on_rate, hpsdr_gain_callback_t on_gain) {
    s_freq_cb = on_freq;
    s_rate_cb = on_rate;
    s_gain_cb = on_gain;

    cyw43_arch_lwip_begin();
    s_pcb = udp_new();
    if (s_pcb) {
        ip_set_option(s_pcb, SOF_BROADCAST);
        udp_bind(s_pcb, IP_ADDR_ANY, HPSDR_PORT);
        udp_recv(s_pcb, hpsdr_recv_callback, NULL);
    }
    cyw43_arch_lwip_end();
}

void openhpsdr_reset_sample_idx(void) {
    s_sample_idx = 0;
}

void openhpsdr_task(void) {
    cyw43_arch_poll();
    if (s_active && !s_no_watchdog) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms - s_last_packet_rx_ms >= 5000) {
            s_active = false;
            s_sample_idx = 0;
            s_sequence = 0;
        }
    }
}

bool openhpsdr_is_active(void) {
    return s_active && (s_host_port != 0);
}

bool openhpsdr_can_send(void) {
    if (!s_active || s_host_port == 0 || s_pcb == NULL) {
        return false;
    }
    cyw43_int_t *ci = (cyw43_int_t *)&cyw43_state;
    if (ci->wlan_flow_control) {
        return false;
    }
    if (ci->wwd_sdpcm_last_bus_data_credit == ci->wwd_sdpcm_packet_transmit_sequence_number) {
        if (cyw43_poll) {
            cyw43_arch_lwip_begin();
            cyw43_poll();
            cyw43_arch_lwip_end();
        }
        if (ci->wwd_sdpcm_last_bus_data_credit == ci->wwd_sdpcm_packet_transmit_sequence_number) {
            return false;
        }
    }
    return true;
}

static void init_hpsdr_packet(void) {
    memset(s_packet_buffer, 0, HPSDR_PACKET_SIZE);
    s_packet_buffer[0] = 0xEF;
    s_packet_buffer[1] = 0xFE;
    s_packet_buffer[2] = HPSDR_DATA_PACKET;  // 0x01 = Data packet
    s_packet_buffer[3] = HPSDR_EP6_ENDPOINT; // 0x06 = EP6 RX I/Q Stream

    // Standard OpenHPSDR Protocol 1 Header
    // Byte 0-1: 0xEFFE
    // Byte 2: Type (0x01 = Data)
    // Byte 3: Endpoint (0x06 = EP6 IQ)
    // Byte 4-7: 32-bit big-endian Sequence Number
    s_packet_buffer[4] = (uint8_t)(s_sequence >> 24);
    s_packet_buffer[5] = (uint8_t)(s_sequence >> 16);
    s_packet_buffer[6] = (uint8_t)(s_sequence >> 8);
    s_packet_buffer[7] = (uint8_t)(s_sequence);
    s_sequence++;

    // Subframe 1 Sync & Status Header (Offset 8)
    s_packet_buffer[8]  = 0x7F;
    s_packet_buffer[9]  = 0x7F;
    s_packet_buffer[10] = 0x7F;
    s_packet_buffer[11] = 0x00;
    s_packet_buffer[12] = 0x00;
    s_packet_buffer[13] = 0x00;
    s_packet_buffer[14] = 0x00;
    s_packet_buffer[15] = 0x00;

    // Subframe 2 Sync & Status Header (Offset 520)
    s_packet_buffer[520] = 0x7F;
    s_packet_buffer[521] = 0x7F;
    s_packet_buffer[522] = 0x7F;
    s_packet_buffer[523] = 0x00;
    s_packet_buffer[524] = 0x00;
    s_packet_buffer[525] = 0x00;
    s_packet_buffer[526] = 0x00;
    s_packet_buffer[527] = 0x00;
}

void openhpsdr_push_samples(const uint32_t *samples, uint32_t count) {
    if (!s_active || s_host_port == 0 || s_pcb == NULL) {
        s_sample_idx = 0;
        return;
    }

    // Fast path: Exact 1-packet buffer (126 stereo samples = 252 words)
    if (count == HPSDR_WORDS_PER_PACKET && s_sample_idx == 0) {
        cyw43_arch_lwip_begin();
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, HPSDR_PACKET_SIZE, PBUF_POOL);
        if (!p) {
            s_pbuf_alloc_failed++;
            cyw43_arch_lwip_end();
            return;
        }

        if (p->next != NULL) {   // payload must be one contiguous 1032-byte segment
            pbuf_free(p);
            s_pbuf_alloc_failed++;
            cyw43_arch_lwip_end();
            return;
        }

        uint8_t *payload = (uint8_t *)p->payload;
        memset(payload, 0, HPSDR_PACKET_SIZE);

        payload[0] = 0xEF;
        payload[1] = 0xFE;
        payload[2] = HPSDR_DATA_PACKET;
        payload[3] = HPSDR_EP6_ENDPOINT;

        payload[4] = (uint8_t)(s_sequence >> 24);
        payload[5] = (uint8_t)(s_sequence >> 16);
        payload[6] = (uint8_t)(s_sequence >> 8);
        payload[7] = (uint8_t)(s_sequence);
        s_sequence++;

        // Subframe 1 Sync & Status Header (Offset 8)
        payload[8]  = 0x7F;
        payload[9]  = 0x7F;
        payload[10] = 0x7F;

        // Subframe 2 Sync & Status Header (Offset 520)
        payload[520] = 0x7F;
        payload[521] = 0x7F;
        payload[522] = 0x7F;

        // Subframe 1: 63 stereo samples (words 0..125)
        for (uint32_t s = 0; s < 63; s++) {
            uint32_t w_i = samples[2 * s + 1]; // Right/Q -> si -> SDR++ .im
            uint32_t w_q = samples[2 * s];     // Left/I  -> sq -> SDR++ .re
            uint32_t offset = 16 + (s * 8);

            payload[offset + 0] = (uint8_t)(w_i >> 24);
            payload[offset + 1] = (uint8_t)(w_i >> 16);
            payload[offset + 2] = (uint8_t)(w_i >> 8);

            payload[offset + 3] = (uint8_t)(w_q >> 24);
            payload[offset + 4] = (uint8_t)(w_q >> 16);
            payload[offset + 5] = (uint8_t)(w_q >> 8);
        }

        // Subframe 2: 63 stereo samples (words 126..251)
        for (uint32_t s = 0; s < 63; s++) {
            uint32_t w_i = samples[126 + (2 * s) + 1];
            uint32_t w_q = samples[126 + (2 * s)];
            uint32_t offset = 528 + (s * 8);

            payload[offset + 0] = (uint8_t)(w_i >> 24);
            payload[offset + 1] = (uint8_t)(w_i >> 16);
            payload[offset + 2] = (uint8_t)(w_i >> 8);

            payload[offset + 3] = (uint8_t)(w_q >> 24);
            payload[offset + 4] = (uint8_t)(w_q >> 16);
            payload[offset + 5] = (uint8_t)(w_q >> 8);
        }

        s_push_calls++;
        uint32_t t0 = time_us_32();
        err_t err = udp_sendto(s_pcb, p, &s_host_ip, s_host_port);
        if (err == ERR_OK) {
            s_pkts_sent++;
        } else {
            s_udp_err++;
        }
        pbuf_free(p);
        cyw43_arch_lwip_end();

        uint32_t dt = time_us_32() - t0;
        if (dt > s_max_send_us) s_max_send_us = dt;
        if (dt > 100000) {
            s_stall_seq = s_sequence;
            s_stall_dt = dt;
        }
        s_last_send_us = dt;
        return;
    }

    // Fallback streaming chunk path for any other count
    for (uint32_t i = 0; i + 1 < count; i += 2) {
        if (s_sample_idx == 0) {
            init_hpsdr_packet();
        }

        uint32_t w_i = samples[i + 1];
        uint32_t w_q = samples[i];

        uint32_t offset;
        if (s_sample_idx < 63) {
            offset = 16 + (s_sample_idx * 8);
        } else {
            offset = 528 + ((s_sample_idx - 63) * 8);
        }

        s_packet_buffer[offset + 0] = (uint8_t)(w_i >> 24);
        s_packet_buffer[offset + 1] = (uint8_t)(w_i >> 16);
        s_packet_buffer[offset + 2] = (uint8_t)(w_i >> 8);

        s_packet_buffer[offset + 3] = (uint8_t)(w_q >> 24);
        s_packet_buffer[offset + 4] = (uint8_t)(w_q >> 16);
        s_packet_buffer[offset + 5] = (uint8_t)(w_q >> 8);

        s_packet_buffer[offset + 6] = 0x00;
        s_packet_buffer[offset + 7] = 0x00;

        s_sample_idx++;

        if (s_sample_idx >= 126) {
            s_push_calls++;
            uint32_t t0 = time_us_32();
            cyw43_arch_lwip_begin();
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, HPSDR_PACKET_SIZE, PBUF_POOL);
            if (p) {
                pbuf_take(p, s_packet_buffer, HPSDR_PACKET_SIZE);
                err_t err = udp_sendto(s_pcb, p, &s_host_ip, s_host_port);
                if (err == ERR_OK) {
                    s_pkts_sent++;
                } else {
                    s_udp_err++;
                }
                pbuf_free(p);
            } else {
                s_pbuf_alloc_failed++;
            }
            cyw43_arch_lwip_end();
            uint32_t dt = time_us_32() - t0;
            if (dt > s_max_send_us) s_max_send_us = dt;
            s_last_send_us = dt;
            s_sample_idx = 0;
        }
    }
}
