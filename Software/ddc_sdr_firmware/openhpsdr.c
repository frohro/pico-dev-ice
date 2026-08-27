#include "openhpsdr.h"
#include <string.h>
#include "pico/cyw43_arch.h"
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

static void send_discovery_reply(const ip_addr_t *addr, u16_t port) {
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 60, PBUF_RAM);
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
    cyw43_arch_lwip_end();
}

static uint32_t s_last_parsed_freq = 0;

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

            // Command 0x01 (VFO/TX) or Command 0x02..0x09 (RX0..RX7)
            if (command_type >= 0x01 && command_type <= 0x09) {
                uint32_t freq_hz = ((uint32_t)c1 << 24) | ((uint32_t)c2 << 16) | ((uint32_t)c3 << 8) | c4;
                if (freq_hz <= 30000000) {
                    target_freq = freq_hz;
                }
            }
            // Command 0x00: General control (sample rate & preamp)
            else if (command_type == 0x00) {
                uint8_t speed = c1 & 0x03;
                uint32_t rate = (speed == 0x01) ? 96000 : 48000;
                if (s_rate_cb) {
                    s_rate_cb(rate);
                }
                // Bit 2: Preamp / Attenuator state in C0
                if (s_gain_cb) {
                    bool preamp = (c0 & 0x04) != 0;
                    s_gain_cb(preamp ? 0x00 : 0x03); // 0x00 = +40 dB, 0x03 = +25 dB
                }
            }
        }
    }

    if (target_freq > 0 && target_freq != s_last_parsed_freq && s_freq_cb) {
        s_last_parsed_freq = target_freq;
        s_freq_cb(target_freq);
    }
}

static void hpsdr_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb;
    if (!p) return;

    uint8_t *data = (uint8_t *)p->payload;

    // Discovery Broadcast: 0xEFFE 0x02
    if (p->len >= 3 && data[0] == 0xEF && data[1] == 0xFE && data[2] == 0x02) {
        send_discovery_reply(addr, port);
    }
    // Start / Stop / Run command: 0xEFFE 0x04
    else if (p->len >= 4 && data[0] == 0xEF && data[1] == 0xFE && data[2] == 0x04) {
        if (data[3] & 0x01) {
            s_active = true;
            ip_addr_copy(s_host_ip, *addr);
            s_host_port = port;
        } else {
            s_active = false;
            s_sequence = 0;
            s_sample_idx = 0;
        }
    }
    // Command & Control (C&C) or standard data: 0xEFFE 0x01
    else if (p->len >= 8 && data[0] == 0xEF && data[1] == 0xFE) {
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
        udp_bind(s_pcb, IP_ADDR_ANY, HPSDR_PORT);
        udp_recv(s_pcb, hpsdr_recv_callback, NULL);
    }
    cyw43_arch_lwip_end();
}

void openhpsdr_task(void) {
    // Background tasks if needed
}

bool openhpsdr_is_active(void) {
    return s_active && (s_host_port != 0);
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
    if (!s_active || s_host_port == 0) {
        s_sample_idx = 0;
        return;
    }

    // Each stereo frame is 2 words: [0]=Left (I), [1]=Right (Q)
    for (uint32_t i = 0; i + 1 < count; i += 2) {
        if (s_sample_idx == 0) {
            init_hpsdr_packet();
        }

        // Swap I and Q for Wi-Fi OpenHPSDR streaming to match SDR++ default orientation
        uint32_t w_i = samples[i + 1];
        uint32_t w_q = samples[i];

        uint32_t offset;
        if (s_sample_idx < 63) {
            offset = 16 + (s_sample_idx * 8); // Subframe 1 data (3B I, 3B Q, 2B mic)
        } else {
            offset = 528 + ((s_sample_idx - 63) * 8); // Subframe 2 data
        }

        // S24 Big-Endian (OpenHPSDR wire format)
        s_packet_buffer[offset + 0] = (uint8_t)(w_i >> 24);
        s_packet_buffer[offset + 1] = (uint8_t)(w_i >> 16);
        s_packet_buffer[offset + 2] = (uint8_t)(w_i >> 8);

        s_packet_buffer[offset + 3] = (uint8_t)(w_q >> 24);
        s_packet_buffer[offset + 4] = (uint8_t)(w_q >> 16);
        s_packet_buffer[offset + 5] = (uint8_t)(w_q >> 8);

        s_packet_buffer[offset + 6] = 0x00; // Mic byte 1
        s_packet_buffer[offset + 7] = 0x00; // Mic byte 2

        s_sample_idx++;

        if (s_sample_idx >= 126) {
            // Full 1032-byte packet ready -> send over UDP
            cyw43_arch_lwip_begin();
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, HPSDR_PACKET_SIZE, PBUF_RAM);
            if (p) {
                pbuf_take(p, s_packet_buffer, HPSDR_PACKET_SIZE);
                udp_sendto(s_pcb, p, &s_host_ip, s_host_port);
                pbuf_free(p);
            }
            cyw43_arch_lwip_end();
            s_sample_idx = 0;
        }
    }
}
