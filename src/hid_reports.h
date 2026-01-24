#ifndef PUSBKB_HID_REPORTS_H
#define PUSBKB_HID_REPORTS_H

// Report IDs must match the HID report descriptor.
#define PUSBKB_REPORT_ID_KEYBOARD 0
#define PUSBKB_REPORT_ID_CONSUMER 1

// HID interface instances.
#define PUSBKB_HID_ITF_KEYBOARD 0
#define PUSBKB_HID_ITF_AUX      1

// UART packet types (0x00-prefixed packets).
typedef enum {
  PUSBKB_PKT_TYPE_KEYBOARD = 0,
  PUSBKB_PKT_TYPE_CONSUMER = 1,
} pusbkb_pkt_type_t;

// Packet type byte: low bits encode type, MSB encodes release.
#define PUSBKB_PKT_FLAG_RELEASE 0x80
#define PUSBKB_PKT_TYPE_MASK    0x0F

// Keyboard flags byte.
#define PUSBKB_KBD_FLAG_APPLE_FN 0x01

#endif /* PUSBKB_HID_REPORTS_H */
