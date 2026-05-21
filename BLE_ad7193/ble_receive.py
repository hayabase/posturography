#!/usr/bin/env python3
"""Receive AD7193 samples from the ESP32 BLE sketch and save them as CSV."""

from __future__ import annotations

import argparse
import asyncio
import csv
import struct
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional


DEFAULT_DEVICE_NAME = "ESP32-AD7193-BLE"
SERVICE_UUID = "7b7f0001-8f4c-4d52-a9f8-9c7d2b1f0001"
DATA_CHAR_UUID = "7b7f0002-8f4c-4d52-a9f8-9c7d2b1f0001"
FRAME = struct.Struct("<IIIII")


@dataclass
class Counters:
    total: int = 0
    one_sec: int = 0
    ten_sec: int = 0
    start_all: float = 0.0
    start_one_sec: float = 0.0
    start_ten_sec: float = 0.0
    last_sample_index: Optional[int] = None
    dropped_by_index: int = 0
    bad_frames: int = 0


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Receive BLE notifications from ESP32-AD7193-BLE."
    )
    parser.add_argument(
        "--name",
        default=DEFAULT_DEVICE_NAME,
        help=f"BLE device name to scan for. Default: {DEFAULT_DEVICE_NAME}",
    )
    parser.add_argument(
        "--address",
        help="Connect directly to this BLE address/UUID instead of scanning by name.",
    )
    parser.add_argument(
        "--csv",
        type=Path,
        help="CSV output path. Default: ble_ad7193_YYYYmmdd_HHMMSS.csv",
    )
    parser.add_argument(
        "--no-save",
        action="store_true",
        help="Do not save CSV; only print receive statistics.",
    )
    parser.add_argument(
        "--duration",
        type=float,
        help="Stop automatically after this many seconds.",
    )
    parser.add_argument(
        "--scan-timeout",
        type=float,
        default=8.0,
        help="BLE scan timeout in seconds. Default: 8",
    )
    parser.add_argument(
        "--print-samples",
        action="store_true",
        help="Print every received sample. This can be noisy at 100 Hz.",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Hide periodic 1-second and 10-second rate messages.",
    )
    return parser


def default_csv_path() -> Path:
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return Path(f"ble_ad7193_{stamp}.csv")


async def find_device(args, BleakScanner):
    if args.address:
        return args.address

    print(f"Scanning for BLE device named '{args.name}' ({args.scan_timeout:.1f}s)...")
    devices = await BleakScanner.discover(timeout=args.scan_timeout)
    matches = [dev for dev in devices if dev.name == args.name]

    if matches:
        dev = matches[0]
        print(f"Found: {dev.name} [{dev.address}]")
        return dev

    print("Target device was not found.")
    if devices:
        print("Visible BLE devices:")
        for dev in devices:
            shown_name = dev.name or "(no name)"
            print(f"  - {shown_name} [{dev.address}]")
    else:
        print("No BLE devices were discovered.")
    return None


def open_csv(path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    f = path.open("w", newline="", encoding="utf-8")
    writer = csv.writer(f)
    writer.writerow(["timestamp", "perf_time_sec", "sample_index", "DAT1", "DAT2", "DAT3", "DAT4"])
    return f, writer


def print_rate(counters: Counters, now: float, quiet: bool) -> None:
    if quiet:
        return

    if now - counters.start_one_sec >= 1.0:
        elapsed = now - counters.start_one_sec
        hz = counters.one_sec / elapsed if elapsed > 0 else 0.0
        print(
            f"[{time.strftime('%H:%M:%S')}] "
            f"1s samples: {counters.one_sec} ({hz:.1f} Hz), "
            f"total: {counters.total}, index drops: {counters.dropped_by_index}"
        )
        counters.one_sec = 0
        counters.start_one_sec = now

    if now - counters.start_ten_sec >= 10.0:
        elapsed = now - counters.start_ten_sec
        hz = counters.ten_sec / elapsed if elapsed > 0 else 0.0
        print(
            f"[{time.strftime('%H:%M:%S')}] "
            f"10s samples: {counters.ten_sec} (avg {hz:.1f} Hz)"
        )
        counters.ten_sec = 0
        counters.start_ten_sec = now


async def receive(args) -> int:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        print("Missing dependency: bleak")
        print("Install it with: pip install bleak")
        return 1

    device = await find_device(args, BleakScanner)
    if device is None:
        return 1

    csv_file = None
    writer = None
    if not args.no_save:
        csv_path = args.csv or default_csv_path()
        csv_file, writer = open_csv(csv_path)
        print(f"CSV output: {csv_path.resolve()}")

    counters = Counters()
    counters.start_all = time.perf_counter()
    counters.start_one_sec = counters.start_all
    counters.start_ten_sec = counters.start_all

    first_samples_to_show = 5

    def handle_notification(_, data: bytearray) -> None:
        nonlocal first_samples_to_show

        now = time.perf_counter()
        if len(data) != FRAME.size:
            counters.bad_frames += 1
            if counters.bad_frames <= 5:
                print(f"[WARN] Bad frame length: {len(data)} bytes")
            return

        sample_index, dat1, dat2, dat3, dat4 = FRAME.unpack(bytes(data))

        if counters.last_sample_index is not None:
            expected = (counters.last_sample_index + 1) & 0xFFFFFFFF
            if sample_index != expected:
                counters.dropped_by_index += (sample_index - expected) & 0xFFFFFFFF
        counters.last_sample_index = sample_index

        counters.total += 1
        counters.one_sec += 1
        counters.ten_sec += 1

        if writer is not None:
            writer.writerow(
                [
                    datetime.now().isoformat(timespec="milliseconds"),
                    f"{now:.6f}",
                    sample_index,
                    dat1,
                    dat2,
                    dat3,
                    dat4,
                ]
            )

        if args.print_samples or first_samples_to_show > 0:
            print(f"{sample_index},{dat1},{dat2},{dat3},{dat4}")
            first_samples_to_show -= 1

        print_rate(counters, now, args.quiet)

    try:
        async with BleakClient(device) as client:
            if not client.is_connected:
                print("Failed to connect.")
                return 1

            print("Connected.")
            try:
                print(f"MTU size: {client.mtu_size}")
            except Exception:
                pass
            print("Starting notifications. Press Ctrl+C to stop.")

            await client.start_notify(DATA_CHAR_UUID, handle_notification)

            if args.duration is None:
                while True:
                    await asyncio.sleep(0.2)
            else:
                await asyncio.sleep(max(0.0, args.duration))

            await client.stop_notify(DATA_CHAR_UUID)
    finally:
        if csv_file is not None:
            csv_file.flush()
            csv_file.close()

    elapsed = max(time.perf_counter() - counters.start_all, 1e-9)
    print(
        "Done. "
        f"total={counters.total}, avg={counters.total / elapsed:.1f} Hz, "
        f"index_drops={counters.dropped_by_index}, bad_frames={counters.bad_frames}"
    )
    return 0


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    try:
        return asyncio.run(receive(args))
    except KeyboardInterrupt:
        print("\nStopped by user.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
