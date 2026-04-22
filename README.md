# TD5 Speedometer

A digital dashboard for the Land Rover Defender TD5, running on an ESP32-P4 with a 4.3" 480×800 IPS display. Reads live ECU data over the K-Line via a K+DCAN USB cable and displays speed, RPM, temperatures, battery voltage, and throttle position using an LVGL UI.

## Hardware

| Component | Details |
|---|---|
| MCU | ESP32-P4 + ESP32-C6 (Function EV Board) |
| Display | 4.3" IPS, 480×800, MIPI-DSI, ST7701 panel |
| Touch | GT911 capacitive touch controller |
| ECU interface | K+DCAN USB cable (K-Line mode) |
| Connection | USB OTG port on the ESP32-P4 board |

## What It Displays

- Vehicle speed (km/h) — GPS-corrected (+5%, ECU reads low)
- Engine RPM
- Coolant temperature (°C)
- Ambient air temperature (°C)
- Battery voltage (V)
- Throttle / fuelling demand (%)

## ECU Protocol

The TD5 ECU uses **ISO 14230 (KWP2000)** framing over K-Line at **10400 baud** with proprietary PIDs — it is **not** standard OBD-II.

```
Tester address : 0xF7
ECU address    : 0x13
Session type   : 0xA0 (diagnostic)
Auth           : seed/key security access (SID 0x27)
```

PIDs polled (verified against a real Defender TD5):

| PID | Data |
|-----|------|
| `0x09` | RPM (16-bit, direct) |
| `0x0D` | Speed (8-bit, km/h) |
| `0x10` | Battery voltage (×0.001 V) |
| `0x1A` | Throttle/fuelling (16 bytes, 8 ADC pairs) |
| `0x1C` | Temperatures (8 bytes: coolant + ambient in °C) |

## Project Structure

```
td5_project/
  td5_speedo/
    main/
      main.c              — app entry point, BSP init, LVGL timer
      td5_display.c/h     — LVGL UI creation and update
      td5_usb.c/h         — USB Host driver, K+DCAN communication
    components/
      td5_protocol/       — message builders, PID parsers, checksum
    managed_components/   — LVGL, BSP (auto-fetched via IDF component manager)
  common_components/
    bsp_extra/            — display brightness / extra BSP helpers
    espressif__esp32_p4_function_ev_board/ — manufacturer BSP

tools/
  td5_kline_test.py       — Python test harness (simulate / parse K-Line logs)
  run_kline_feed.bat      — Windows helper to run the test feed
  run_td5_test.bat        — Windows helper to run the test suite
  logs/                   — captured K-Line session logs
```

## Building & Flashing

Requires **ESP-IDF v5.3+**.

```bash
cd td5_project/td5_speedo
idf.py set-target esp32p4
idf.py build
idf.py -p <PORT> flash monitor
```

## Wiring / Setup

1. Connect the K+DCAN cable to the **USB OTG port** (not the UART port) via a USB-C OTG adapter.
2. Set the cable switch to **K-LINE** (left position).
3. Turn the vehicle ignition **ON**.
4. Flash and boot the ESP32-P4 — it will auto-connect and begin polling.

## Tools

`tools/td5_kline_test.py` can replay captured K-Line logs for offline development:

```bash
python tools/td5_kline_test.py --log tools/logs/<log_file>.log
```

## References

- [EA2EGA/Ekaitza_Itzali](https://github.com/EA2EGA/Ekaitza_Itzali)
- [BennehBoy/LRDuinoTD5](https://github.com/BennehBoy/LRDuinoTD5)
- [pajacobson/td5keygen](https://github.com/pajacobson/td5keygen)
