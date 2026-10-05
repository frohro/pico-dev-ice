#include "openhpsdr.h"
#include <stdio.h>
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

static uint32_t s_push_calls = 0;
static uint32_t s_pkts_sent = 0;
static uint32_t s_pbuf_alloc_failed = 0;
static uint32_t s_udp_err = 0;
static uint32_t s_max_send_us = 0;
static uint32_t s_last_send_us = 0;

void openhpsdr_get_stats(uint32_t *push_calls, uint32_t *pkts_sent, uint32_t *pbuf_failed, uint32_t *udp_err, uint32_t *max_us, uint32_t *last_us, uint32_t *stall_seq, uint32_t *stall_dt) {
    if (push_calls) *push_calls = s_push_calls;
    if (pkts_sent) *pkts_sent = s_pkts_sent;
    if (pbuf_failed) *pbuf_failed = s_pbuf_alloc_failed;
    if (udp_err) *udp_err = s_udp_err;
    if (max_us) *max_us = s_max_send_us;
    if (last_us) *last_us = s_last_send_us;
    if (stall_seq) *stall_seq = 0;
    if (stall_dt) *stall_dt = 0;
}

static void send_discovery_reply(const ip_addr_t *addr, u16_t port) {
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 60, PBUF_POOL);
    if (p) {
        uint8_t *payload = (uint8_t *)p->payload;
        memset(payload, 0, 60);

        payload[0] = 0xEF; payload[1] = 0xFE; payload[2] = 0x02; 
        
        uint8_t mac[6] = {0};
        cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
        memcpy(&payload[3], mac, 6);

        payload[9]  = 0x21; // Firmware v3.3
        payload[10] = 0x01; // Board ID: Hermes
        payload[11] = 0x01; // Protocol 1
        payload[12] = 0x01; // 1 RX
        payload[13] = 0x01; // 1 ADC

        err_t err = udp_sendto(s_pcb, p, addr, port);
        pbuf_free(p);
        printf("[HPSDR] Discovery reply sent to %s:%d (err=%d)\n", ip4addr_ntoa(addr), port, err);
    }
}

static void handle_cc_packet(const uint8_t *data, uint16_t len) {
    if (len < 8) return;

    for (int offset = 8; offset + 7 < (int)len; offset += 512) {
        if (data[offset] == 0x7F && data[offset+1] == 0x7F && data[offset+2] == 0x7F) {
            uint8_t c0 = data[offset + 3];
            uint8_t c1 = data[offset + 4];
            uint8_t c2 = data[offset + 5];
            uint8_t c3 = data[offset + 6];
            uint8_t c4 = data[offset + 7];

            uint8_t command_type = (c0 >> 1) & 0x3F;

            if (command_type >= 0x01 && command_type <= 0x09) {
                uint32_t freq_hz = ((uint32_t)c1 << 24) | ((uint32_t)c2 << 16) | ((uint32_t)c3 << 8) | c4;
                if (freq_hz <= 30000000 && s_freq_cb) {
                    s_freq_cb(freq_hz);
                }
            }
            else if (command_type == 0x0A && s_gain_cb) {
                uint8_t raw_gain = c4 & 0x3F; 
                uint8_t pga_code = (raw_gain >= 60) ? 0 : (uint8_t)(15 - ((uint32_t)raw_gain * 15 / 60));
                s_gain_cb(pga_code);
            }
            else if (command_type == 0x00) {
                if (s_rate_cb) {
                    uint8_t speed = c1 & 0x03;
                    s_rate_cb((speed == 0x01) ? 96000 : 48000);
                }
                if (s_gain_cb) {
                    s_gain_cb((c0 & 0x04) ? 0x00 : 0x03); 
                }
            }
        }
    }
}

static volatile bool s_no_watchdog = false;
static uint32_t s_last_packet_rx_ms = 0;

static void hpsdr_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb;
    if (!p) return;

    s_last_packet_rx_ms = to_ms_since_boot(get_absolute_time());
    uint8_t *data = (uint8_t *)p->payload;

    if (p->len >= 3 && data[0] == 0xEF && data[1] == 0xFE) {
        if (data[2] == 0x02) {
            send_discovery_reply(addr, port);
        }
        else if (data[2] == 0x04) {
            if (data[3] & 0x01) {
                if (!s_active) s_sequence = 0;
                s_active = true;
                s_no_watchdog = (data[3] & 0x80) != 0;
                ip_addr_copy(s_host_ip, *addr);
                s_host_port = port;
            } else {
                s_active = false;
                s_no_watchdog = false;
                s_host_port = 0;
                s_sequence = 0;
            }
        }
        else if (data[2] == 0x01) {
            if (!s_active) s_sequence = 0;
            s_active = true;
            ip_addr_copy(s_host_ip, *addr);
            s_host_port = port;
            handle_cc_packet(data, p->len);
        }
        else if (data[2] == 0xBB) {
            pbuf_free(p);
            reset_usb_boot(0, 0);
            return;
        }
        else if (data[2] == 0xCC) {
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
                memcpy(rp->payload, stats, sizeof(stats));
                udp_sendto(s_pcb, rp, addr, port);
                pbuf_free(rp);
            }
        }
    }

    pbuf_free(p);
}

void openhpsdr_init(hpsdr_freq_callback_t on_freq, hpsdr_rate_callback_t on_rate, hpsdr_gain_callback_t on_gain) {
    s_freq_cb = on_freq;
    s_rate_cb = on_rate;
    s_gain_cb = on_gain;

    s_pcb = udp_new();
    if (s_pcb) {
        ip_set_option(s_pcb, SOF_BROADCAST);
        udp_bind(s_pcb, IP_ADDR_ANY, HPSDR_PORT);
        udp_recv(s_pcb, hpsdr_recv_callback, NULL);
    }
}

void openhpsdr_task(void) {
    if (s_active && !s_no_watchdog) {
        if (to_ms_since_boot(get_absolute_time()) - s_last_packet_rx_ms >= 5000) {
            s_active = false;
            s_sequence = 0;
        }
    }
}

bool openhpsdr_is_active(void) {
    return s_active && (s_host_port != 0);
}

bool openhpsdr_can_send(void) {
    return (s_active && s_host_port != 0 && s_pcb != NULL);
}

void openhpsdr_reset_sample_idx(void) {
}

struct udp_pcb *openhpsdr_get_pcb(void) {
    return s_pcb;
}

// NOTE: Now returns bool for backpressure tracking!
bool openhpsdr_push_samples(const uint32_t *samples, uint32_t count) {
    if (!openhpsdr_can_send()) return false;

    if (count == HPSDR_WORDS_PER_PACKET) {
        // Polled mode: NO lwip_begin / lwip_end needed here!
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, HPSDR_PACKET_SIZE, PBUF_POOL);
        if (!p) {
            s_pbuf_alloc_failed++;
            return false; // Backpressure! Tell caller to try again later.
        }

        uint8_t *payload = (uint8_t *)p->payload;
        memset(payload, 0, HPSDR_PACKET_SIZE);

        payload[0] = 0xEF; payload[1] = 0xFE; payload[2] = HPSDR_DATA_PACKET; payload[3] = HPSDR_EP6_ENDPOINT;
        payload[4] = (uint8_t)(s_sequence >> 24); payload[5] = (uint8_t)(s_sequence >> 16);
        payload[6] = (uint8_t)(s_sequence >> 8);  payload[7] = (uint8_t)(s_sequence);
        s_sequence++;

        payload[8] = 0x7F; payload[9] = 0x7F; payload[10] = 0x7F;
        payload[520] = 0x7F; payload[521] = 0x7F; payload[522] = 0x7F;

        // Fast unpack
        for (uint32_t s = 0; s < 63; s++) {
            uint32_t w_i = samples[2 * s + 1]; 
            uint32_t w_q = samples[2 * s];     
            uint32_t offset = 16 + (s * 8);
            payload[offset+0] = w_i>>24; payload[offset+1] = w_i>>16; payload[offset+2] = w_i>>8;
            payload[offset+3] = w_q>>24; payload[offset+4] = w_q>>16; payload[offset+5] = w_q>>8;
        }

        for (uint32_t s = 0; s < 63; s++) {
            uint32_t w_i = samples[126 + (2 * s) + 1];
            uint32_t w_q = samples[126 + (2 * s)];
            uint32_t offset = 528 + (s * 8);
            payload[offset+0] = w_i>>24; payload[offset+1] = w_i>>16; payload[offset+2] = w_i>>8;
            payload[offset+3] = w_q>>24; payload[offset+4] = w_q>>16; payload[offset+5] = w_q>>8;
        }

        s_push_calls++;
        uint32_t t0 = time_us_32();
        
        err_t err = udp_sendto(s_pcb, p, &s_host_ip, s_host_port);
        
        if (err == ERR_OK) {
            s_pkts_sent++;
            pbuf_free(p);
            uint32_t dt = time_us_32() - t0;
            if (dt > s_max_send_us) s_max_send_us = dt;
            s_last_send_us = dt;
            return true;
        } else {
            s_udp_err++;
            pbuf_free(p);
            s_sequence--; // Rollback sequence because we failed
            return false; // Backpressure! Tell caller to try again later.
        }
    }
    return true; 
}
