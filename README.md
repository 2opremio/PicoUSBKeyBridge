# PicoUSBKeyBridge

`PicoUSBKeyBridge` is a bridge firmware for Raspberry Pi Pico-class boards. It
accepts keypress packets over UART and exposes a USB HID keyboard to a target
host over the board’s USB port.

It lets you programmatically send keypress events to a host that supports USB
HID keyboards.

Use cases:
- Automate kiosk or demo devices that only accept USB keyboards.
- Drive hardware test rigs that need deterministic input events.
- Programmatically emulate keyboard keypresses from software workflows.
- Remotely send keypresses to a closed device that doesn't allow remote control (e.g., iPhone, iPad).

## What you need

- A Raspberry Pi Pico-class board (RP2040/RP2350).
- A USB-to-UART serial adapter (3.3V logic).
- This firmware.

## Example hardware

I am using the [Waveshare RP2350-USB-A](https://www.waveshare.com/rp2350-usb-a.htm)
with an [FT232 adapter](https://www.az-delivery.de/en/products/ftdi-adapter-ft232rl).
The default wiring below matches that board’s header and the adapter’s labeled pins.

![My setup (annotated)](img/mysetup-annotated.jpeg)

Note: the Waveshare RP2350-USB-A USB-A port on that board is **unused** in this firmware; the USB-C port
is the HID device port. I originally picked the Waveshare RP2350-USB-A because I
wanted to use the [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)
stack. PIO USB failed to enumerate reliably on macOS/iPadOS in my setup (see
[issue #196](https://github.com/sekigon-gonnoc/Pico-PIO-USB/issues/196)), so I
moved to TinyUSB over USB-C for HID and used the UART adapter for control/logging.

## Build

1. Install toolchain dependencies:
   - CMake
   - Ninja
   - GNU Arm Embedded toolchain (`arm-none-eabi-gcc`)

2. Initialize submodules:
```
git submodule update --init --recursive
```

3. Configure:
```
cmake -S . -B build
```

4. Build:
```
cmake --build build
```

UF2 output is in `build/` (e.g. `build/PicoUSBKeyBridge.uf2`).

## Flash

1. Hold **BOOT** and connect the board over USB-C.
2. A mass-storage device appears (BOOTSEL).
3. Copy `PicoUSBKeyBridge.uf2` to the BOOTSEL drive.

## Wiring checklist

- Connect the board’s **USB port** to the **target host** (enumerates as a HID keyboard).
- Connect a USB-to-UART adapter to the board UART pins.
- Make sure the adapter is set to **3.3V logic**.

Default UART wiring (configurable):

- Adapter **RX** → `GPIO4` (UART TX)
- Adapter **TX** → `GPIO5` (UART RX)
- Adapter **GND** → any **GND** on the board
See `UART configuration` below to change the pin mapping.

## Serial protocol

The UART interface expects a fixed 2-byte packet at 115200 baud:

- **Byte 0**: USB HID keycode
- **Byte 1**: modifier bitmap

USB HID keyboard keycodes are defined in the HID Usage Tables (Keyboard/Keypad page)
and in TinyUSB’s `hid.h` constants.
- HID Usage Tables (Keyboard/Keypad): https://usb.org/sites/default/files/hut1_4.pdf
- TinyUSB keycode definitions: https://github.com/hathach/tinyusb/blob/6e891c6dc716d6ae91fdc54aaec2899f788e14fc/src/class/hid/hid.h#L389-L391

Modifier bitmap matches the USB HID keyboard modifier bits (macOS symbols):

- `0x01` Left Ctrl (⌃)
- `0x02` Left Shift (⇧)
- `0x04` Left Alt / Option (⌥)
- `0x08` Left GUI / Command (⌘)
- `0x10` Right Ctrl (⌃)
- `0x20` Right Shift (⇧)
- `0x40` Right Alt / Option (⌥)
- `0x80` Right GUI / Command (⌘)

Each packet generates a key press followed by a key release.

For example, this sequence:

```
04 00  04 02  1E 08
```

Corresponds to the keypress sequence [`a`, `A`, `⌘+1`]

Step-by-step:

1. `04 00` → HID keycode `0x04` corresponding to letter `a`, no modifier (produces `a`).
2. `04 02` → HID keycode `0x04` corresponding to letter `a`, with Left Shift `0x02` (produces `A`).
3. `1E 08` → HID keycode `0x1E` corresponding to `1`, with Left GUI/Command `0x08` (produces `⌘+1`).


UART TX is reserved for **logs only**. The device never sends protocol bytes back,
so the host can safely read TX output as plain text logs.

## USB bridge daemon

This repo also ships a small HTTP daemon that keeps a persistent connection to
the PicoUSBKeyBridge UART adapter, retries forever, and logs device output to
stdout.

Build/run:

```
go run ./cmd/picousbridged
```

macOS deployment:

```
./scripts/deploy_macos.sh
```

Other systems:

- Linux: use `systemd` (not included yet).
- Windows: TBD.

Flags:

- `-host` (default: `localhost`)
- `-port` (default: `8080`)
- `-send-timeout` (default: `2`) seconds to wait when queueing a keypress
- `-vid` (default: `0x0403`) USB VID for the serial adapter
- `-pid` (default: `0x6001`) USB PID for the serial adapter

## HTTP API

`POST /keypress` with a low-level HID keycode + modifier flags.

For instance, to send letter `A` (`a` (HID code 4) + `Shift`) :

```
curl -X POST "http://localhost:8080/keypress" \
  -H "Content-Type: application/json" \
  -d '{"hid_code":4,"left_shift":true}'
```

Response:

```
{"status":"ok"}
```

Notes:

- `hid_code` is a USB HID Usage ID (Keyboard/Keypad page).
- HID modifier flags are optional boolean fields on the request:
  - `left_ctrl`, `left_shift`, `left_alt`, `left_gui`
  - `right_ctrl`, `right_shift`, `right_alt`, `right_gui`

## Client library

There is a small Go client in `client/` for calling the HTTP API.

```
client := usbbridge.New(usbbridge.Config{
	Host: "localhost:8080",
})
err := client.SendKeypress(ctx, usbbridge.KeypressRequest{HIDCode: 0x04, LeftShift: true}) // A with Shift
```

## UART configuration

Override UART pins/baud rate at build time:

```
cmake -S . -B build \
  -DPUSBKB_UART_INDEX=1 \
  -DPUSBKB_UART_TX_PIN=4 \
  -DPUSBKB_UART_RX_PIN=5 \
  -DPUSBKB_UART_BAUDRATE=115200
```