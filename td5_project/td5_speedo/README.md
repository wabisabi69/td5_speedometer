# TD5 Speedometer — Defender Edition

Digital speedometer and engine gauge for the **Land Rover Defender TD5**, running on
an **ESP32-P4 + ESP32-C6** dev board with 4.3" 480×800 IPS capacitive touchscreen.

Reads live data directly from the TD5 ECU via a **K+DCAN USB cable** plugged into
the ESP32-P4's USB OTG port.

## Hardware Required

| Component | Notes |
|-----------|-------|
| ESP32-P4 + ESP32-C6 board | 4.3" 480×800 IPS, ST7701 panel, GT911 touch |
| K+DCAN USB cable (FTDI) | OBD2-to-USB with K-line/DCAN switch |
| USB-C OTG adapter | USB-A female → USB-C male |
| 12V→5V buck converter | Powers ESP32-P4 from OBD2 pin 16 |

## Directory Setup

Your manufacturer's SDK comes with a `common_components` folder containing the BSP.
This project **must sit alongside that folder**:

```
your_sdk_folder/
├── common_components/          ← from manufacturer SDK (contains BSP)
│   ├── espressif__esp32_p4_function_ev_board/
│   ├── espressif__esp_lcd_st7701/
│   └── ...
├── lvgl_demo_v9/               ← manufacturer demo (for reference)
└── td5_speedo/                 ← THIS PROJECT (put it here)
    ├── CMakeLists.txt
    ├── main/
    ├── components/
    └── ...
```

## Building

```bash
cd td5_speedo

# Set target
idf.py set-target esp32p4

# Build
idf.py build

# Flash via UART USB-C port (NOT the OTG port)
idf.py -p /dev/ttyUSB0 flash monitor
```

## Wiring

```
TD5 OBD2 Port              K+DCAN Cable              ESP32-P4 Board
┌──────────┐             ┌──────────────┐          ┌──────────────┐
│ Pin 7  K ─────────────│ FTDI FT232RL │── USB ──│ USB OTG port │
│ Pin 4  GND ───────────│ + transceiver│          │              │
│ Pin 5  GND ───────────│              │          │              │
│ Pin 16 +12V ─┐        └──────────────┘          │              │
└──────────┘   │  ┌──────────┐                    │              │
               └──│ 12V→5V   │── USB-C ──────────│ Power port   │
                  └──────────┘                    └──────────────┘
```

**Cable switch → K-LINE position (LEFT)**

## What Data You Get

| Gauge | Source |
|-------|--------|
| Vehicle Speed (km/h) | ECU fuelling data, offset 26-27 |
| Engine RPM | ECU fuelling data, offset 0-1 |
| Coolant Temperature | ECU fuelling data, offset 20-21 |
| Battery Voltage | ECU fuelling data, offset 22-23 |
| Throttle Position | ECU fuelling data, offset 16-17 |
| Boost Pressure (PSI) | MAP sensor via ECU, offset 6-7 |

## Calibration

Speed conversion: `raw / 128.0 → km/h`. Calibrate against GPS and adjust
the divisor in `components/td5_protocol/td5_protocol.c` if needed.

## Troubleshooting

**"FTDI not found"** — Wrong USB port. Use the OTG port, not the UART/debug port.
Need a USB-A→USB-C OTG adapter.

**"No init response"** — Cable switch wrong (must be LEFT/K-line). Or ignition off.
Or OBD2 wiring issue (check pin 7 K-line continuity to ECU).

**"Authentication FAILED"** — Try swapping seed high/low bytes in `td5_protocol.c`.
Both MSB and NNN type ECUs should work.

**Display blank** — Make sure `common_components` folder is at `../common_components`
relative to this project. That's where the BSP lives.

## Project Structure

```
td5_speedo/
├── CMakeLists.txt              # References ../common_components BSP
├── sdkconfig.defaults          # Board-specific: PSRAM, MIPI-DSI, USB, fonts
├── partitions.csv
├── main/
│   ├── main.c                  # BSP display init + USB + LVGL timer
│   ├── td5_usb.c/.h           # USB Host CDC-ACM → FTDI cable comms
│   ├── td5_display.c/.h       # LVGL speedometer gauge (480×800)
│   └── idf_component.yml      # LVGL 9.2, ST7701, USB CDC-ACM deps
└── components/
    └── td5_protocol/
        ├── td5_keygen.c        # ECU seed→key authentication
        ├── td5_protocol.c      # Message building, parsing, checksums
        └── include/
            ├── td5_keygen.h
            └── td5_protocol.h  # Constants, PID offsets, data structs
```

## Credits

- [td5keygen](https://github.com/pajacobson/td5keygen) — Auth algorithm (BSD-2-Clause)
- [Ekaitza_Itzali](https://github.com/EA2EGA/Ekaitza_Itzali) — Protocol documentation
- [LRDuinoTD5](https://github.com/BennehBoy/LRDuinoTD5) — Arduino TD5 gauge system
