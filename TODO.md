# TODO

- [ ] Verify USB pull-ups on PIO USB-A port
  - Multimeter mode: **resistance (ohms)**, board **unpowered**.
  - 3.3V testpoint: **P1 pin 15 (3V3)** on the header.
  - GND testpoint: **P1 pin 16 (GND)** on the header.
  - Measure **D+ (USB-A pin 3)** to **3.3V**: expect about **1.5kΩ**.
  - Measure **D- (USB-A pin 2)** to **3.3V**: expect **open / very high**.
  - If both D+ and D- measure ~1.5kΩ, both pull-ups are populated (not desired).
  - Optional powered check (voltage mode): D+ ~3.3V to GND, D- ~0V to GND.
