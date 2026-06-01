# GoldenJoy BLE Mouse

GoldenJoy is Bluetooth HID mouse firmware for an ESP32-C3 and a Nintendo Wii Nunchuck controller.

The goal is to provide a small, low-cost, adaptable pointing device for accessibility use. A Nunchuck can be easier to hold or mount than a conventional mouse, and its joystick plus two buttons make it a useful starting point for people who benefit from thumb-based pointer control, custom positioning, or reduced hand and arm movement.

This is an experimental assistive technology project. It is meant to be tuned, remapped, mounted, and adapted to the person using it.

## What It Does

- Reads a Nintendo Nunchuck over I2C.
- Advertises as a Bluetooth LE HID mouse named `GoldenJoy Mouse`.
- Converts joystick motion into relative mouse movement.
- Maps the Nunchuck buttons to mouse clicks.
- Calibrates joystick center at boot.
- Exposes simple firmware constants for pointer speed, deadzone, direction, and button behavior.

## Reference Project

This project was inspired by [esp32beans/USBnunchuck_mouse](https://github.com/esp32beans/USBnunchuck_mouse), which converts a Wii Nunchuck into a USB HID mouse using CircuitPython and Adafruit hardware.

Key differences:

- GoldenJoy uses Bluetooth LE HID; the reference project uses USB HID.
- GoldenJoy targets ESP32-C3 with Arduino/PlatformIO; the reference project uses CircuitPython on boards such as QT Py ESP32-S3 and RP2040 variants.
- GoldenJoy manually reads the Nunchuck over I2C; the reference project uses Adafruit's CircuitPython Nunchuck library.
- GoldenJoy calibrates joystick center at boot; the reference project assumes the joystick center is near 127.
- GoldenJoy currently uses a conservative linear pointer response; the reference project uses an acceleration lookup table.

## Hardware

- ESP32-C3 development board
- Nintendo Wii Nunchuck controller
- Nunchuck breakout or adapter cable
- 3.3 V power only. Do not feed the Nunchuck 5 V.

Default wiring:

| Nunchuck | ESP32-C3 |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO 8 |
| SCL | GPIO 9 |

If your ESP32-C3 board exposes different convenient I2C pins, update `kSdaPin` and `kSclPin` in `src/main.cpp`.

## Controls

- Joystick: mouse movement
- Z button: left click
- C button: right click

On boot, leave the joystick untouched for about one second while the firmware calibrates the resting center.

## Accessibility Tuning

The main tuning constants are near the top of `src/main.cpp`:

- `kDeadzone`: raises or lowers the neutral area around the joystick center
- `kPointerGain`: adjusts pointer speed
- `kMaxStep`: caps maximum movement per report
- `kInvertX` / `kInvertY`: flips movement direction
- `kZIsLeftClick` / `kCIsRightClick`: changes button mapping behavior

Start with `kPointerGain` before changing anything else. Lower values make the pointer calmer; higher values make it faster. Increase `kDeadzone` if the cursor drifts while the joystick is at rest.

## Build And Upload

This project uses PlatformIO:

```sh
python3 -m platformio run
python3 -m platformio run --target upload
```

After flashing, pair the device named `GoldenJoy Mouse` from the host computer or tablet Bluetooth settings.

## Status

The firmware builds for `esp32-c3-devkitm-1` and implements the basic mouse behavior. Real-world accessibility use will need person-specific tuning and hardware mounting work.

## License

This project is licensed under the MIT License. See `LICENSE` for details.
