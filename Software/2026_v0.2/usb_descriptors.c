/*
 * usb_descriptors.c — Research: 48/96 kHz PCM1808 UAC1 SDR (v0.2)
 *
 * Interface layout:
 *  IAD 1  CDC/ACM
 *    Interface 0 — CDC Control, EP 0x83 IN interrupt 8 B
 *    Interface 1 — CDC Data,    EP 0x04 OUT bulk 64 B, EP 0x84 IN bulk 64 B
 *  IAD 2  Audio
 *    Interface 2 — Audio Control (no EPs)
 *      Signal chain: IT(1,Mic) -> FU(2,mute) -> OT(3,USB)
 *    Interface 3 alt 0 — zero bandwidth
 *    Interface 3 alt 1 —  48 kHz, 2ch, 24-bit S24_3LE, EP max 294 B
 *    Interface 3 alt 2 —  96 kHz, 2ch, 24-bit S24_3LE, EP max 582 B
 *
 * AUDIO_BLOCK_LEN byte count:
 *    8   manual Audio IAD
 *    9   TUD_AUDIO10_DESC_STD_AC_LEN
 *    9   TUD_AUDIO10_DESC_CS_AC_LEN(1)
 *   12   TUD_AUDIO10_DESC_INPUT_TERM_LEN
 *   13   TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)
 *    9   TUD_AUDIO10_DESC_OUTPUT_TERM_LEN
 *    9   alt 0  zero-bw STD_AS
 *   43   alt 1  (STD_AS + CS_AS + FORMAT(1) + STD_EP + CS_EP)
 *   43   alt 2
 *        = 155
 */

#include "bsp/board_api.h"
#include "tusb.h"

#define _PID_MAP(itf, n)  ( (CFG_TUD_##itf) << (n) )
#define USB_VID   0xCafe
#define USB_PID   (0x4000 | _PID_MAP(CDC,0) | _PID_MAP(MSC,1) | _PID_MAP(HID,2) \
                          | _PID_MAP(MIDI,3) | _PID_MAP(AUDIO,4) | _PID_MAP(VENDOR,5))

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

/* Interface numbers */
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_AUDIO_CONTROL,    /* 2 */
    ITF_NUM_AUDIO_STREAMING,  /* 3 */
    ITF_NUM_TOTAL             /* 4 */
};

#define EPNUM_AUDIO_IN    0x81   /* EP1 IN  isochronous audio */
#define EPNUM_CDC_NOTIF   0x83   /* EP3 IN  CDC notification  */
#define EPNUM_CDC_OUT     0x04   /* EP4 OUT CDC bulk          */
#define EPNUM_CDC_IN      0x84   /* EP4 IN  CDC bulk          */

#define AUDIO_CS_AC_TOTALLEN \
    (TUD_AUDIO10_DESC_INPUT_TERM_LEN + \
     TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) + \
     TUD_AUDIO10_DESC_OUTPUT_TERM_LEN)

/* One active-alternate block = STD_AS + CS_AS + FORMAT(1) + STD_EP + CS_EP */
#define AUDIO_ONE_ALT_LEN ( \
    TUD_AUDIO10_DESC_STD_AS_LEN           + \
    TUD_AUDIO10_DESC_CS_AS_INT_LEN        + \
    TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN    + \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN       \
)

/* alt 0: zero-bw STD_AS only; alts 1-2: full active-alternate block */
#define AUDIO_BLOCK_LEN ( \
    8u                                   + \
    TUD_AUDIO10_DESC_STD_AC_LEN          + \
    TUD_AUDIO10_DESC_CS_AC_LEN(1)        + \
    TUD_AUDIO10_DESC_INPUT_TERM_LEN      + \
    TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) + \
    TUD_AUDIO10_DESC_OUTPUT_TERM_LEN     + \
    TUD_AUDIO10_DESC_STD_AS_LEN          + \
    AUDIO_ONE_ALT_LEN                    + \
    AUDIO_ONE_ALT_LEN                      \
)

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + AUDIO_BLOCK_LEN)

#define STRIDX_CDC_IF    4

static const uint8_t desc_configuration[] = {
    /* Configuration */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    /* CDC (IAD 1) */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRIDX_CDC_IF,
                       EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    /* Audio IAD (8 bytes, manual — no TUD macro for UAC1 IAD) */
    8, TUSB_DESC_INTERFACE_ASSOCIATION,
    ITF_NUM_AUDIO_CONTROL,
    2,
    TUSB_CLASS_AUDIO,
    0x00,
    0x00,
    0x00,

    /* Audio Control Interface */
    TUD_AUDIO10_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, 0x00),

    /* CS AC Header — IT(12)+FU(13)+OT(9)=34 */
    TUD_AUDIO10_DESC_CS_AC(0x0100, AUDIO_CS_AC_TOTALLEN, ITF_NUM_AUDIO_STREAMING),

    /* Input Terminal: ID=1, Mic, assocOT=3, 2ch L+R */
    TUD_AUDIO10_DESC_INPUT_TERM(0x01,
                                AUDIO_TERM_TYPE_IN_GENERIC_MIC,
                                0x03, 2,
                                (AUDIO10_CHANNEL_CONFIG_LEFT_FRONT |
                                 AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT),
                                0x00, 0x00),

    /* Feature Unit: ID=2, src=IT(1), master mute only */
    TUD_AUDIO10_DESC_FEATURE_UNIT(0x02, 0x01, 0x00,
                                  AUDIO10_FU_CONTROL_BM_MUTE,
                                  0x0000, 0x0000),

    /* Output Terminal: ID=3, USB Streaming, assocIT=1, src=FU(2) */
    TUD_AUDIO10_DESC_OUTPUT_TERM(0x03, AUDIO_TERM_TYPE_USB_STREAMING,
                                 0x01, 0x02, 0x00),

    /* AS alt 0: zero bandwidth */
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x00, 0x00, 0x00),

    /* AS alt 1: 48 kHz, 24-bit S24_3LE  (294 B = (48+1)*3*2) */
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x01, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(0x03, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    11, TUSB_DESC_CS_INTERFACE, 0x02, 0x01,  2, 3, 24, 1,  0x80, 0xBB, 0x00,
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN,
        (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS),
        294, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
        AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),

    /* AS alt 2: 96 kHz, 24-bit S24_3LE  (582 B = (96+1)*3*2) */
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x02, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(0x03, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    11, TUSB_DESC_CS_INTERFACE, 0x02, 0x01,  2, 3, 24, 1,  0x00, 0x77, 0x01,
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_IN,
        (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS),
        582, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ,
        AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

/*─────────────────────────────────────────────────────────────────────
 * String Descriptors
 *─────────────────────────────────────────────────────────────────────*/
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC_IF,
};

static const char *const string_desc_arr[] = {
    [STRID_LANGID]       = (const char[]){ 0x09, 0x04 },
    [STRID_MANUFACTURER] = "WWU CPTR 480",
    [STRID_PRODUCT]      = "SDR PCM1808 2026",
    [STRID_SERIAL]       = NULL,         /* filled by board_usb_get_serial() */
    [STRID_CDC_IF]       = "LO Control",
};

static uint16_t _desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    if (index == STRID_LANGID) {
        memcpy(&_desc_str[1], string_desc_arr[STRID_LANGID], 2);
        chr_count = 1;
    } else if (index == STRID_SERIAL) {
        chr_count = board_usb_get_serial(_desc_str + 1, 32);
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
