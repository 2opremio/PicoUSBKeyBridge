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
#ifdef CFG_TUSB_DEBUG
#undef CFG_TUSB_DEBUG
#endif
#define CFG_TUSB_DEBUG            1  // 0=off, 1=errors, 2=warnings, 3=info, 4=debug

// Route TinyUSB debug logs to UART TX.
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
#define CFG_TUD_CDC              0
#define CFG_TUD_HID              1

// Used by HID descriptor sizing.
#define CFG_TUD_HID_EP_BUFSIZE   64

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
