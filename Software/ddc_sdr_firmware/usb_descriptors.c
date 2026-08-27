#include <string.h>

#include "tusb.h"

#define ITF_NUM_CDC 0
#define ITF_NUM_CDC_DATA 1
#define ITF_NUM_AUDIO_CONTROL 2
#define ITF_NUM_AUDIO_CAPTURE 3
#define ITF_NUM_AUDIO_PLAYBACK 4
#define ITF_NUM_DFU 5
#define ITF_NUM_TOTAL 6

#define EPNUM_AUDIO_IN 0x81
#define EPNUM_AUDIO_OUT 0x01
#define EPNUM_CDC_NOTIF 0x83
#define EPNUM_CDC_OUT 0x04
#define EPNUM_CDC_IN 0x84

#define AUDIO_CS_AC_TOTALLEN \
    (TUD_AUDIO10_DESC_INPUT_TERM_LEN + \
     TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) + \
    TUD_AUDIO10_DESC_OUTPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_INPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_OUTPUT_TERM_LEN)

#define AUDIO_ONE_ALT_LEN ( \
    TUD_AUDIO10_DESC_STD_AS_LEN + \
    TUD_AUDIO10_DESC_CS_AS_INT_LEN + \
    TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN + \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN)

#define AUDIO_BLOCK_LEN ( \
    8u + \
    TUD_AUDIO10_DESC_STD_AC_LEN + \
    TUD_AUDIO10_DESC_CS_AC_LEN(2) + \
    TUD_AUDIO10_DESC_INPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) + \
    TUD_AUDIO10_DESC_OUTPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_INPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_OUTPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_STD_AS_LEN + \
    AUDIO_ONE_ALT_LEN + \
    AUDIO_ONE_ALT_LEN + \
    TUD_AUDIO10_DESC_STD_AS_LEN + \
    AUDIO_ONE_ALT_LEN + \
    AUDIO_ONE_ALT_LEN)

#define DDC_CONFIGURATION_LEN ( \
    TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + AUDIO_BLOCK_LEN + \
    TUD_DFU_DESC_LEN(CFG_TUD_DFU_ALT))

#ifndef CONFIG_TOTAL_LEN
#define CONFIG_TOTAL_LEN DDC_CONFIGURATION_LEN
#endif

#include "pico/unique_id.h"
#include "pico/bootrom.h"

_Static_assert(DDC_CONFIGURATION_LEN == CONFIG_TOTAL_LEN,
               "CONFIG_TOTAL_LEN does not match the DDC descriptor");

/* Device Descriptor */
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x1209,
    .idProduct          = 0xb1c0,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

const uint8_t tud_desc_configuration[CONFIG_TOTAL_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    8, TUSB_DESC_INTERFACE_ASSOCIATION,
    ITF_NUM_AUDIO_CONTROL, 3, TUSB_CLASS_AUDIO, 0x00, 0x00, 0x00,
    TUD_AUDIO10_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, 0x00),
    10, TUSB_DESC_CS_INTERFACE, AUDIO10_CS_AC_INTERFACE_HEADER,
    0x00, 0x01, (uint8_t)(AUDIO_CS_AC_TOTALLEN + 10u),
    (uint8_t)((AUDIO_CS_AC_TOTALLEN + 10u) >> 8), 0x02,
    ITF_NUM_AUDIO_CAPTURE, ITF_NUM_AUDIO_PLAYBACK,
    TUD_AUDIO10_DESC_INPUT_TERM(0x01, AUDIO_TERM_TYPE_IN_GENERIC_MIC,
                                0x03, 2,
                                AUDIO10_CHANNEL_CONFIG_LEFT_FRONT |
                                AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT,
                                0x00, 0x00),
    TUD_AUDIO10_DESC_FEATURE_UNIT(0x02, 0x01, 0x00,
                                  AUDIO10_FU_CONTROL_BM_MUTE,
                                  0x0000, 0x0000),
    TUD_AUDIO10_DESC_OUTPUT_TERM(0x03, AUDIO_TERM_TYPE_USB_STREAMING,
                                 0x01, 0x02, 0x00),

    TUD_AUDIO10_DESC_INPUT_TERM(0x11, AUDIO_TERM_TYPE_USB_STREAMING,
                                0x13, 2,
                                AUDIO10_CHANNEL_CONFIG_LEFT_FRONT |
                                AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT,
                                0x00, 0x00),
    TUD_AUDIO10_DESC_OUTPUT_TERM(0x13, AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER,
                                 0x00, 0x11, 0x00),

    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_CAPTURE, 0x00, 0x00, 0x00),

    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_CAPTURE, 0x01, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(0x03, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    11, TUSB_DESC_CS_INTERFACE, 0x02, 0x01, 2, 3, 24, 1,
    0x80, 0xbb, 0x00,
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN,
        TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS,
        294, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(
        AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
        AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),

    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_CAPTURE, 0x02, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(0x03, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    11, TUSB_DESC_CS_INTERFACE, 0x02, 0x01, 2, 3, 24, 1,
    0x00, 0x77, 0x01,
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN,
        TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS,
        582, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(
        AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
        AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),

    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_PLAYBACK, 0x00, 0x00, 0x00),

    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_PLAYBACK, 0x01, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(0x11, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    11, TUSB_DESC_CS_INTERFACE, 0x02, 0x01, 2, 3, 24, 1,
    0x80, 0xbb, 0x00,
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT,
        TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE,
        294, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(
        AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
        AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),

    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_PLAYBACK, 0x02, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(0x11, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    11, TUSB_DESC_CS_INTERFACE, 0x02, 0x01, 2, 3, 24, 1,
    0x00, 0x77, 0x01,
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT,
        TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE,
        582, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(
        AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
        AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),

    TUD_DFU_DESCRIPTOR(ITF_NUM_DFU, CFG_TUD_DFU_ALT, 5,
                       DFU_ATTR_CAN_DOWNLOAD, 1000, CFG_TUD_DFU_XFER_BUFSIZE),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return tud_desc_configuration;
}

static const char *const string_desc_arr[] = {
    [0] = (const char[]){ 0x09, 0x04 }, // 0: English
    [1] = USB_MANUFACTURER,             // 1: Manufacturer
    [2] = USB_PRODUCT,                  // 2: Product
    [3] = NULL,                         // 3: Serial
    [4] = "SDR Control",                // 4: CDC
    [5] = "iCE40 DFU (CRAM)",           // 5: DFU
};

static uint16_t _desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else if (index == 3) {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(serial, sizeof(serial));
        chr_count = strlen(serial);
        if (chr_count > 32) chr_count = 32;
        for (size_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = (uint16_t)serial[i];
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
            return NULL;
        const char *str = string_desc_arr[index];
        if (!str) return NULL;
        chr_count = strlen(str);
        if (chr_count > 32) chr_count = 32;
        for (size_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = (uint16_t)str[i];
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

void tud_dfu_download_cb(uint8_t alt, uint16_t block_num, const uint8_t *data, uint16_t length)
{
    (void)alt; (void)block_num; (void)data; (void)length;
}

void tud_dfu_manifest_cb(uint8_t alt)
{
    (void)alt;
}

void tud_dfu_abort_cb(uint8_t alt)
{
    (void)alt;
}

void tud_dfu_detach_cb(void)
{
    reset_usb_boot(0, 0);
}

uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state)
{
    (void)alt; (void)state;
    return 0;
}
