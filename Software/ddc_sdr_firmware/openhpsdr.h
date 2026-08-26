#ifndef DDC_OPENHPSDR_H
#define DDC_OPENHPSDR_H

#include <stdint.h>
#include <stdbool.h>
#include "lwip/udp.h"

#define HPSDR_PORT              1024
#define HPSDR_PACKET_SIZE       1032
#define HPSDR_SYNC_WORD         0xEFFE
#define HPSDR_DATA_PACKET       0x01
#define HPSDR_EP6_ENDPOINT      0x06
#define HPSDR_DISCOVERY_RESP    0x02

// Callback function types when host changes frequency, sample rate, or PGA gain
typedef void (*hpsdr_freq_callback_t)(uint32_t freq_hz);
typedef void (*hpsdr_rate_callback_t)(uint32_t rate_hz);
typedef void (*hpsdr_gain_callback_t)(uint8_t pga_code);

void openhpsdr_init(hpsdr_freq_callback_t on_freq, hpsdr_rate_callback_t on_rate, hpsdr_gain_callback_t on_gain);
void openhpsdr_task(void);

// Push raw 32-bit words (I/Q stereo pairs from FPGA I2S) into OpenHPSDR Protocol 1 packets
void openhpsdr_push_samples(const uint32_t *samples, uint32_t count);

bool openhpsdr_is_active(void);

#endif // DDC_OPENHPSDR_H
