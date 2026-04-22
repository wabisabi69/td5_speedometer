#!/usr/bin/env python3
"""
TD5 K-Line Data Test v5 - corrected parsing from real ECU data

PID response formats (verified against actual Defender TD5):

PID 0x09 (RPM):
  Response: 04 61 09 [HI] [LO] [CS]
  RPM = (HI<<8 | LO)  (direct, no scaling needed)
  Idle example: 02 E7 = 743 RPM ✓

PID 0x0D (Speed):
  Response: 03 61 0D [SPD] [CS]
  Speed = SPD in km/h (single byte, direct)
  Parked example: 00 = 0 km/h ✓

PID 0x10 (Battery):
  Response: 06 61 10 [B1H] [B1L] [B2H] [B2L] [CS]
  Battery V = (B1H<<8 | B1L) / 1000.0
  Example: 35 1E = 13598 -> 13.6V ✓

PID 0x1A (Throttle/Fuelling - 16 data bytes):
  Response: 12 61 1A [d0..d15] [CS]
  8 pairs of 16-bit values:
    Pair 0: Driver demand (raw ADC)
    Pair 1: Throttle idle/reference (raw ADC, ~928 at idle)
    Pair 2-7: Other fuelling params (injection qty, EGR, wastegate, etc.)
  Throttle % ≈ (pair0 - pair1) / pair1 * 100  (0% at idle)

PID 0x1C (Temperatures - 8 data bytes):
  Response: 0A 61 1C [d0..d7] [CS]
  4 pairs of 16-bit values:
    Pair 0: Coolant sensor raw ADC
    Pair 1: Fuel temp sensor raw ADC
    Pair 2: Coolant temperature in °C (DIRECT, no formula)
    Pair 3: Ambient air temperature in °C (DIRECT, no formula)
  Idle 2-3min example: coolant=44°C, ambient=26°C ✓
"""

import argparse
import logging
import os
import sys
import time
from datetime import datetime
import serial

BAUD_RATE = 10400
TESTER_ADDR = 0xF7
ECU_ADDR = 0x13

SID_START_DIAG = 0x10
SID_SECURITY_ACCESS = 0x27
SID_READ_DATA = 0x21
SID_TESTER_PRESENT = 0x3E
REQUEST_SEED = 0x01
SEND_KEY = 0x02
DIAG_SESSION = 0xA0

PID_RPM = 0x09
PID_SPEED = 0x0D
PID_BATTERY = 0x10
PID_THROTTLE = 0x1A
PID_TEMPS = 0x1C

POLL_PIDS = [
    ("RPM",      PID_RPM),
    ("Speed",    PID_SPEED),
    ("Battery",  PID_BATTERY),
    ("Throttle", PID_THROTTLE),
    ("Temps",    PID_TEMPS),
]

P3_DELAY = 0.025


def setup_logging(log_dir="logs"):
    os.makedirs(log_dir, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_file = os.path.join(log_dir, f"td5_v5_{ts}.log")
    logger = logging.getLogger("td5")
    logger.setLevel(logging.DEBUG)
    fh = logging.FileHandler(log_file, encoding="utf-8")
    fh.setLevel(logging.DEBUG)
    fh.setFormatter(logging.Formatter("%(asctime)s.%(msecs)03d %(levelname)-8s %(message)s",
                                       datefmt="%H:%M:%S"))
    logger.addHandler(fh)
    ch = logging.StreamHandler()
    ch.setLevel(logging.INFO)
    ch.setFormatter(logging.Formatter("%(message)s"))
    logger.addHandler(ch)
    logger.info(f"Log: {log_file}")
    return logger

log = logging.getLogger("td5")

def checksum(data):
    return sum(data) & 0xFF

def build_msg(*b):
    data = bytes(b)
    return data + bytes([checksum(data)])

def verify_cs(data):
    return len(data) >= 2 and data[-1] == checksum(data[:-1])

def td5_keygen(seed):
    seed = seed & 0xFFFF
    count = ((seed >> 0xC & 0x8) + (seed >> 0x5 & 0x4) + (seed >> 0x3 & 0x2) + (seed & 0x1)) + 1
    for _ in range(count):
        tap = ((seed >> 1) + (seed >> 2) + (seed >> 8) + (seed >> 9)) & 1
        tmp = (seed >> 1) | (tap << 0xF)
        if (seed >> 0x3 & 1) and (seed >> 0xD & 1):
            seed = tmp & ~1
        else:
            seed = tmp | 1
    return seed & 0xFFFF

def hx(data):
    return " ".join(f"{b:02X}" for b in data) if data else "(empty)"

def msg_init():
    return build_msg(0x81, ECU_ADDR, TESTER_ADDR, 0x81)

def msg_start():
    return build_msg(0x02, SID_START_DIAG, DIAG_SESSION)

def msg_seed():
    return build_msg(0x02, SID_SECURITY_ACCESS, REQUEST_SEED)

def msg_key(seed):
    k = td5_keygen(seed)
    return build_msg(0x04, SID_SECURITY_ACCESS, SEND_KEY, (k >> 8) & 0xFF, k & 0xFF)

def msg_read_pid(pid):
    return build_msg(0x02, SID_READ_DATA, pid)

def msg_alive():
    return build_msg(0x02, SID_TESTER_PRESENT, 0x01)


def drain(ser):
    ser.timeout = 0.01
    junk = ser.read(256)
    if junk:
        log.debug(f"DRAIN: {hx(junk)}")

def send_recv(ser, data, timeout=2.0, raw=False):
    drain(ser)
    log.debug(f"TX: {hx(data)}")
    if raw:
        print(f"  TX: {hx(data)}")
    ser.write(data)
    ser.flush()
    ser.timeout = 0.3
    echo = ser.read(len(data))
    log.debug(f"ECHO: {hx(echo)}")
    if raw and echo:
        print(f"  ECHO: {hx(echo)}")
    time.sleep(0.005)
    ser.timeout = timeout
    b0 = ser.read(1)
    if not b0:
        if raw:
            print("  RX: (timeout)")
        return b""
    first = b0[0]
    if first >= 0x80:
        hdr_rest = ser.read(2)
        plen = first & 0x3F
        payload = ser.read(plen) if plen > 0 else b""
        cs = ser.read(1)
        result = b0 + hdr_rest + payload + cs
    else:
        rest = ser.read(first + 1)
        result = b0 + rest
    log.debug(f"RX: {hx(result)}")
    if raw:
        print(f"  RX: {hx(result)}")
    return result

def fast_init(ser, raw=False):
    log.info("Fast init (25ms break)...")
    ser.break_condition = True
    time.sleep(0.025)
    ser.break_condition = False
    time.sleep(0.025)
    ser.baudrate = BAUD_RATE
    drain(ser)


def get_word(d, offset):
    """Get 16-bit big-endian value from data at offset."""
    if len(d) > offset + 1:
        return (d[offset] << 8) | d[offset + 1]
    return None


def parse_pid(pid, data):
    """Parse a positive PID response into a dict of named values."""
    if not data or len(data) < 3 or data[1] != 0x61 or not verify_cs(data):
        return None

    d = data[3:-1]  # payload bytes only (skip len, 0x61, pid; drop checksum)
    r = {}

    if pid == PID_RPM:
        # 2 bytes: RPM direct
        v = get_word(d, 0)
        if v is not None:
            r["rpm"] = v

    elif pid == PID_SPEED:
        # 1 byte: speed in km/h direct, corrected by 1.04
        if len(d) >= 1:
            r["speed_kph"] = round(d[0] * 1.04)

    elif pid == PID_BATTERY:
        # 4 bytes: 2 pairs, first pair = battery voltage * 1000
        v = get_word(d, 0)
        if v is not None:
            r["battery_v"] = v / 1000.0

    elif pid == PID_THROTTLE:
        # 16 bytes: 8 pairs
        # Pair 0 = driver demand (raw ADC)
        # Pair 1 = throttle idle reference (raw ADC)
        demand = get_word(d, 0)
        idle_ref = get_word(d, 2)
        if demand is not None and idle_ref is not None and idle_ref > 0:
            # Throttle % relative to idle
            # At idle: demand ≈ idle_ref * 3.6 (based on 3451/928)
            # Full range appears to be ~928 (idle) to ~4096 (WOT) for pair 1
            # Pair 0 (demand) at idle ≈ 3451, at WOT would be higher
            # Simple approach: use pair 1 as the actual pedal position
            # percentage = (pair1 - idle_min) / (wot_max - idle_min) * 100
            # With observed idle ~928 and assuming WOT ~4096:
            r["throttle_raw"] = idle_ref
            r["throttle_pct"] = max(0.0, (idle_ref - 900) / (4096 - 900) * 100.0)
            r["demand_raw"] = demand

        # Also extract injection quantity (pair 4) and other useful values
        inj_qty = get_word(d, 8)
        if inj_qty is not None:
            r["inj_qty"] = inj_qty / 10.0  # mg/stroke approx

    elif pid == PID_TEMPS:
        # 8 bytes: 4 pairs
        # Pair 0: coolant sensor raw ADC
        # Pair 1: fuel temp sensor raw ADC  
        # Pair 2: coolant temperature in °C (DIRECT)
        # Pair 3: ambient air temperature in °C (DIRECT)
        coolant = get_word(d, 4)   # pair 2
        ambient = get_word(d, 6)   # pair 3
        coolant_raw = get_word(d, 0)  # pair 0 (raw ADC)
        fuel_raw = get_word(d, 2)     # pair 1 (raw ADC)

        if coolant is not None:
            r["coolant_c"] = coolant
        if ambient is not None:
            r["ambient_c"] = ambient
        if coolant_raw is not None:
            r["coolant_raw"] = coolant_raw
        if fuel_raw is not None:
            r["fuel_temp_raw"] = fuel_raw

    return r if r else None


def main():
    global log
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM8")
    parser.add_argument("--raw", action="store_true")
    parser.add_argument("--count", type=int, default=0)
    parser.add_argument("--retries", type=int, default=3)
    args = parser.parse_args()

    log = setup_logging()
    log.info("=" * 60)
    log.info(f"TD5 K-Line Test v5 | {args.port} | raw={args.raw}")
    log.info("=" * 60)

    poll_count = 0
    ser = serial.Serial(port=args.port, baudrate=BAUD_RATE,
                        bytesize=8, parity="N", stopbits=1, timeout=2.0)

    try:
        # ── Handshake ────────────────────────────────────────
        authenticated = False
        for attempt in range(1, args.retries + 1):
            log.info(f"\n--- Attempt {attempt}/{args.retries} ---")
            fast_init(ser, args.raw)

            log.info("Init frame...")
            resp = send_recv(ser, msg_init(), 2.0, args.raw)
            if not resp:
                log.error("No init response")
                time.sleep(1)
                continue
            log.info(f"  Init OK: {hx(resp)}")
            time.sleep(P3_DELAY)

            log.info("Start diag...")
            resp = send_recv(ser, msg_start(), 2.0, args.raw)
            if not resp or len(resp) < 2 or resp[1] != 0x50:
                log.error(f"  Bad: {hx(resp)}")
                time.sleep(1)
                continue
            log.info("  Diag session OK")
            time.sleep(P3_DELAY)

            log.info("Seed request...")
            resp = send_recv(ser, msg_seed(), 2.0, args.raw)
            if not resp or len(resp) < 6 or resp[1] != 0x67:
                log.error(f"Bad seed: {hx(resp)}")
                time.sleep(1)
                continue
            seed = (resp[3] << 8) | resp[4]
            log.info(f"  Seed: 0x{seed:04X}  Key: 0x{td5_keygen(seed):04X}")
            time.sleep(P3_DELAY)

            log.info("Send key...")
            resp = send_recv(ser, msg_key(seed), 2.0, args.raw)
            if resp and len(resp) >= 3 and resp[1] == 0x67 and resp[2] == 0x02:
                log.info("  *** AUTHENTICATED ***")
                authenticated = True
                break
            log.error(f"  Auth failed: {hx(resp)}")
            time.sleep(1)

        if not authenticated:
            log.error("Failed all retries")
            sys.exit(1)

        time.sleep(P3_DELAY)

        # ── Test PIDs ────────────────────────────────────────
        log.info("\nTesting PIDs...")
        working = []
        for name, pid in POLL_PIDS:
            resp = send_recv(ser, msg_read_pid(pid), 0.5, args.raw)
            if resp and len(resp) >= 3 and resp[1] == 0x61:
                working.append((name, pid))
                log.info(f"  0x{pid:02X} ({name}): OK [{len(resp)} bytes]")
            else:
                log.warning(f"  0x{pid:02X} ({name}): not supported")
            time.sleep(P3_DELAY)

        if not working:
            log.error("No PIDs responded!")
            sys.exit(1)

        log.info(f"\nPolling: {[n for n,_ in working]}")

        # ── Live data ────────────────────────────────────────
        log.info("\n" + "=" * 70)
        log.info("  LIVE DATA  (Ctrl+C to stop)")
        log.info("=" * 70)
        log.info(f"  {'#':>5}  {'RPM':>5}  {'km/h':>5}  {'Cool':>5}  {'Amb':>4}  {'Batt':>5}  {'Thr%':>5}  {'InjMg':>5}")
        log.info("  " + "-" * 60)

        ka = 0
        while True:
            vals = {}
            for name, pid in working:
                resp = send_recv(ser, msg_read_pid(pid), 0.5, args.raw)
                if resp:
                    p = parse_pid(pid, resp)
                    if p:
                        vals.update(p)
                time.sleep(P3_DELAY)

            if vals:
                poll_count += 1
                rpm = vals.get("rpm", "")
                spd = vals.get("speed_kph", "")
                cool = vals.get("coolant_c", "")
                amb = vals.get("ambient_c", "")
                bat = vals.get("battery_v", "")
                thr = vals.get("throttle_pct", "")
                inj = vals.get("inj_qty", "")

                parts = []
                parts.append(f"{poll_count:5d}")
                parts.append(f"{rpm:>5}" if rpm != "" else "    -")
                parts.append(f"{spd:>5}" if spd != "" else "    -")
                parts.append(f"{cool:>5}" if cool != "" else "    -")
                parts.append(f"{amb:>4}" if amb != "" else "   -")
                parts.append(f"{bat:>5.1f}" if bat != "" else "    -")
                parts.append(f"{thr:>5.1f}" if thr != "" else "    -")
                parts.append(f"{inj:>5.1f}" if inj != "" else "    -")

                line = "  " + "  ".join(parts)
                log.info(line)

                # Also log readable summary every 10 polls
                if poll_count % 10 == 1:
                    summary = []
                    if "rpm" in vals:
                        summary.append(f"RPM:{vals['rpm']}")
                    if "speed_kph" in vals:
                        summary.append(f"Speed:{vals['speed_kph']}km/h")
                    if "coolant_c" in vals:
                        summary.append(f"Coolant:{vals['coolant_c']}°C")
                    if "ambient_c" in vals:
                        summary.append(f"Ambient:{vals['ambient_c']}°C")
                    if "battery_v" in vals:
                        summary.append(f"Battery:{vals['battery_v']:.1f}V")
                    if "throttle_pct" in vals:
                        summary.append(f"Throttle:{vals['throttle_pct']:.1f}%")
                    log.info("  >> " + " | ".join(summary))

            ka += 1
            if ka >= 10:
                ka = 0
                send_recv(ser, msg_alive(), 0.5, False)
            if args.count and poll_count >= args.count:
                break

    except KeyboardInterrupt:
        print()
        log.info("Stopped.")
    except serial.SerialException as e:
        log.error(f"Serial: {e}")
    finally:
        ser.close()
        log.info(f"Done. {poll_count} polls.")

if __name__ == "__main__":
    main()
