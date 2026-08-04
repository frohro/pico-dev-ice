#ifndef DDC_PROTOCOL_H_
#define DDC_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#define DDC_FPGA_SYNC 0xD5u
#define DDC_FPGA_PROTOCOL_VERSION 1u
#define DDC_FPGA_FRAME_HEADER_LEN 4u
#define DDC_FPGA_MAX_FREQUENCY_HZ 30000000u
#define DDC_PGA_MAX_CODE 0x0fu

enum ddc_fpga_command {
    DDC_FPGA_CMD_SET_FREQUENCY = 0x01,
    DDC_FPGA_CMD_SET_SAMPLE_RATE = 0x02,
    DDC_FPGA_CMD_GET_STATUS = 0x03,
    DDC_FPGA_CMD_CLEAR_OTR = 0x04
};

/* The automatic overload path uses only verified gain states. */
static inline uint8_t ddc_pga_next_otr_code(uint8_t pga_code)
{
    switch (pga_code & DDC_PGA_MAX_CODE) {
    case 0x0u:
        return 0x1u;
    case 0x1u:
        return 0x3u;
    case 0x3u:
        return 0xfu;
    default:
        return 0xfu;
    }
}

static inline void ddc_put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static inline uint32_t ddc_get_le32(const uint8_t *src)
{
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

static inline size_t ddc_make_u32_command(uint8_t *frame,
                                           uint8_t command,
                                           uint32_t value)
{
    frame[0] = DDC_FPGA_SYNC;
    frame[1] = DDC_FPGA_PROTOCOL_VERSION;
    frame[2] = command;
    frame[3] = 4u;
    ddc_put_le32(frame + DDC_FPGA_FRAME_HEADER_LEN, value);
    return DDC_FPGA_FRAME_HEADER_LEN + 4u;
}

#endif
