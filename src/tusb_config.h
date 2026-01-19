/*
 * TinyUSB configuration for PicoUSBKeyBridge.
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

// --------------------------------------------------------------------
// COMMON CONFIGURATION
// --------------------------------------------------------------------

#define CFG_TUSB_OS               OPT_OS_PICO
#define CFG_TUD_ENABLED           1
#define CFG_TUH_ENABLED           0
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG            3  // 0=off, 1=errors, 2=warnings, 3=info
#endif

// Route TinyUSB debug logs to CDC TX.
#define CFG_TUSB_DEBUG_PRINTF log_tusb_debug_printf

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

// --------------------------------------------------------------------
// DEVICE CONFIGURATION
// --------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- CLASS -------------//
#define CFG_TUD_CDC              1
#define CFG_TUD_HID              0

// CDC FIFO size of TX and RX
#define CFG_TUD_CDC_RX_BUFSIZE   256
#define CFG_TUD_CDC_TX_BUFSIZE   256

// CDC Endpoint transfer buffer size
#define CFG_TUD_CDC_EP_BUFSIZE   64

// Used by PIO USB HID descriptor sizing.
#define CFG_TUD_HID_EP_BUFSIZE   64

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
