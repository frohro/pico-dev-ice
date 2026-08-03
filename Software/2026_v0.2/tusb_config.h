/*
 * tusb_config.h — Research: 48/96/192 kHz Multi-Rate Multi-ADC UAC1 SDR
 *
 * Audio endpoint sizing for 192 kHz S16_LE stereo (worst case):
 *   EP_IN_SZ_MAX = 772: (192+1) * 2 bytes * 2 channels.
 *   EP_IN_SW_BUF_SZ = 9264: 12 intervals * 772 bytes (~12 ms headroom).
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Board / RHPort
//--------------------------------------------------------------------+

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED   OPT_MODE_DEFAULT_SPEED
#endif

//--------------------------------------------------------------------+
// Common TinyUSB configuration
//--------------------------------------------------------------------+

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------+
// Device configuration
//--------------------------------------------------------------------+

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- Class drivers -------------//
#define CFG_TUD_CDC              1
#define CFG_TUD_MSC              0
#define CFG_TUD_HID              0
#define CFG_TUD_MIDI             0
#define CFG_TUD_AUDIO            1
#define CFG_TUD_VENDOR           0

//--------------------------------------------------------------------+
// Audio Class Driver Configuration
//--------------------------------------------------------------------+

// One Audio Streaming interface (with alts 0-3 for idle/48k/96k/192k)
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT            1
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ        64
#define CFG_TUD_AUDIO_ENABLE_EP_IN               1

// 192 kHz S16_LE stereo: (192+1)*2*2 = 772 bytes
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX       772
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ    9264

//--------------------------------------------------------------------+
// CDC FIFO sizes
//--------------------------------------------------------------------+

#define CFG_TUD_CDC_RX_BUFSIZE   64
#define CFG_TUD_CDC_TX_BUFSIZE   64
#define CFG_TUD_CDC_EP_BUFSIZE   64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
