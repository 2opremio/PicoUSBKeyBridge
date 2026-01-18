/*
 * TinyUSB CDC descriptors for native USB (USB-C) interface.
 */

#include <string.h>

#include "tusb.h"

/* A combination of interfaces must have a unique product id. */
#define _PID_MAP(itf, n)  ( (CFG_TUD_##itf) << (n) )
#define USB_PID           (0x4000 | _PID_MAP(CDC, 0))

#define USB_VID   0xCafe
#define USB_BCD   0x0200

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device =
{
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = USB_BCD,

  // Use Interface Association Descriptor (IAD) for CDC
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

uint8_t const * tud_descriptor_device_cb(void)
{
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum
{
  ITF_NUM_CDC = 0,
  ITF_NUM_CDC_DATA,
  ITF_NUM_TOTAL
};

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

uint8_t const desc_fs_configuration[] =
{
  // Config number, interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

  // Interface number, string index, EP notification address and size, EP data address (out, in) and size.
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index;
  return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

char const* string_desc_arr [] =
{
  (const char[]) { 0x09, 0x04 }, // 0: supported language is English (0x0409)
  "keyemu",                      // 1: Manufacturer
  "keyemu CDC",                  // 2: Product
  "000000000001",                // 3: Serials (placeholder)
  "keyemu CDC",                  // 4: CDC Interface
};

static uint16_t _desc_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void) langid;

  uint8_t chr_count;

  if ( index == 0)
  {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  }else
  {
    if ( !(index < sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) ) return NULL;

    const char* str = string_desc_arr[index];

    chr_count = (uint8_t) strlen(str);
    if ( chr_count > 31 ) chr_count = 31;

    for(uint8_t i=0; i<chr_count; i++)
    {
      _desc_str[1+i] = str[i];
    }
  }

  _desc_str[0] = (TUSB_DESC_STRING << 8 ) | (2*chr_count + 2);
  return _desc_str;
}
