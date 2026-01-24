/*
 * TinyUSB HID descriptors for native USB (USB-C) interface.
 */

#include <string.h>

#include "class/hid/hid_device.h"
#include "tusb.h"

#include "hid_reports.h"

#define USB_VID   0x1915 // Nordic Semiconductor
#define USB_PID   0xEEEF // Nordic HID keyboard sample PID
#define USB_BCD   0x0200

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = USB_BCD,

  // Device class is specified per interface for HID
  .bDeviceClass       = 0x00,
  .bDeviceSubClass    = 0x00,
  .bDeviceProtocol    = 0x00,

  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor           = USB_VID,
  .idProduct          = USB_PID,
  .bcdDevice          = 0x0100,

  .iManufacturer      = 0x01,
  .iProduct           = 0x02,
  .iSerialNumber      = 0x03,

  .bNumConfigurations = 0x01
};

// TinyUSB looks up these descriptor callbacks by symbol name at link time.
uint8_t const * tud_descriptor_device_cb(void) {
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum {
  ITF_NUM_HID_KEYBOARD = 0,
  ITF_NUM_HID_AUX,
  ITF_NUM_TOTAL
};

#define EPNUM_HID_KEYBOARD   0x81
#define EPNUM_HID_AUX        0x82

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)

static uint8_t const desc_hid_report_keyboard[] = {
  // Keyboard report with Apple Fn in the reserved byte.
  // Reference: https://gist.github.com/fauxpark/010dcf5d6377c3a71ac98ce37414c6c4
  0x05, 0x01,                     // Usage Page (Generic Desktop)
  0x09, 0x06,                     // Usage (Keyboard)
  0xA1, 0x01,                     // Collection (Application)
  0x05, 0x07,                     // Usage Page (Key Codes)
  0x19, 0xE0,                     // Usage Minimum (224)
  0x29, 0xE7,                     // Usage Maximum (231)
  0x15, 0x00,                     // Logical Minimum (0)
  0x25, 0x01,                     // Logical Maximum (1)
  0x75, 0x01,                     // Report Size (1)
  0x95, 0x08,                     // Report Count (8)
  0x81, 0x02,                     // Input (Data, Var, Abs) Modifier byte

  0x05, 0xFF,                     // Usage Page (AppleVendor Top Case)
  0x09, 0x03,                     // Usage (KeyboardFn)
  0x15, 0x00,                     // Logical Minimum (0)
  0x25, 0x01,                     // Logical Maximum (1)
  0x75, 0x08,                     // Report Size (8)
  0x95, 0x01,                     // Report Count (1)
  0x81, 0x02,                     // Input (Data, Var, Abs) Apple Fn byte

  0x95, 0x06,                     // Report Count (6)
  0x75, 0x08,                     // Report Size (8)
  0x15, 0x00,                     // Logical Minimum (0)
  0x25, 0x65,                     // Logical Maximum (101)
  0x05, 0x07,                     // Usage Page (Key Codes)
  0x19, 0x00,                     // Usage Minimum (0)
  0x29, 0x65,                     // Usage Maximum (101)
  0x81, 0x00,                     // Input (Data, Array) Key array
  0xC0                            // End Collection
};

static uint8_t const desc_hid_report_aux[] = {
  TUD_HID_REPORT_DESC_CONSUMER( HID_REPORT_ID(PUSBKB_REPORT_ID_CONSUMER) ),
};

uint8_t const desc_fs_configuration[] = {
  // Config number, interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

  // Interface number, string index, protocol, report descriptor len, EP addr, size, interval
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_KEYBOARD, 4, HID_ITF_PROTOCOL_KEYBOARD,
                     sizeof(desc_hid_report_keyboard), EPNUM_HID_KEYBOARD,
                     CFG_TUD_HID_EP_BUFSIZE, 10),

  // Aux HID interface (consumer reports).
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_AUX, 5, HID_ITF_PROTOCOL_NONE,
                     sizeof(desc_hid_report_aux), EPNUM_HID_AUX,
                     CFG_TUD_HID_EP_BUFSIZE, 10),
};

uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
  (void) index;
  return desc_fs_configuration;
}

uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance) {
  if (instance == PUSBKB_HID_ITF_KEYBOARD) {
    return desc_hid_report_keyboard;
  }
  return desc_hid_report_aux;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

char const* string_desc_arr [] = {
  (const char[]) { 0x09, 0x04 }, // 0: supported language is English (0x0409)
  "Nordic Semiconductor",        // 1: Manufacturer
  "Nordic HID Keyboard",         // 2: Product
  "000000000001",                // 3: Serials (placeholder)
  "Nordic HID Keyboard",         // 4: HID Interface (keyboard)
  "Nordic HID Keyboard Aux",     // 5: HID Interface (consumer)
};

static uint16_t _desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;

  uint8_t chr_count;

  if ( index == 0) {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  } else {
    if ( !(index < sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) ) return NULL;

    const char* str = string_desc_arr[index];

    chr_count = (uint8_t) strlen(str);
    if ( chr_count > 31 ) chr_count = 31;

    for(uint8_t i=0; i<chr_count; i++) {
      _desc_str[1+i] = str[i];
    }
  }

  _desc_str[0] = (TUSB_DESC_STRING << 8 ) | (2*chr_count + 2);
  return _desc_str;
}
