#!/usr/bin/env python3
"""
Print a RAM/flash summary and top RAM symbols from the linker map.

Usage:
  ./memory_report.py
  ./memory_report.py --elf cmake-build-debug/PicoUSBKeyBridge.elf
  ./memory_report.py --map cmake-build-debug/PicoUSBKeyBridge.elf.map --top 15
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path
import subprocess
from typing import Dict, Iterable, List, Tuple


PICO_RAM_BYTES = 520 * 1024
# Waveshare RP2350-USB-A ships with 2 MB flash.
PICO_FLASH_BYTES = 2 * 1024 * 1024
PICO_DEVICE_NAME = "RP2350 (Waveshare)"

SECTION_TOTALS = {
    ".boot2",
    ".text",
    ".rodata",
    ".binary_info",
    ".ram_vector_table",
    ".data",
    ".bss",
    ".heap",
    ".stack1_dummy",
    ".stack_dummy",
}


def format_bytes(value: int) -> str:
    if value >= 1024:
        return f"{value} bytes ({value / 1024:.1f} KB)"
    return f"{value} bytes"


def parse_section_totals(lines: Iterable[str]) -> Dict[str, int]:
    totals: Dict[str, int] = {}
    for line in lines:
        match = re.match(
            r"^\s*(\.\S+)\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\b", line
        )
        if not match:
            continue
        name = match.group(1)
        if name not in SECTION_TOTALS:
            continue
        size = int(match.group(2), 16)
        if size > totals.get(name, 0):
            totals[name] = size
    return totals


def get_section_totals_from_size(elf_path: Path) -> Dict[str, int]:
    try:
        result = subprocess.run(
            ["arm-none-eabi-size", "-A", str(elf_path)],
            check=True,
            capture_output=True,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return {}

    totals: Dict[str, int] = {}
    for line in result.stdout.splitlines():
        match = re.match(r"^(\.\S+)\s+(\d+)\s+0x[0-9a-fA-F]+$", line.strip())
        if not match:
            continue
        name = match.group(1)
        if name not in SECTION_TOTALS:
            continue
        totals[name] = int(match.group(2))
    return totals


def parse_bss_data_entries(lines: List[str]) -> List[Tuple[int, str, str]]:
    entries: List[Tuple[int, str, str]] = []
    for idx, line in enumerate(lines[:-1]):
        match = re.match(r"^\s*(\.(bss|data)\.\S+)\s*$", line)
        if not match:
            continue
        name = match.group(1)
        next_line = lines[idx + 1]
        match_next = re.match(
            r"^\s*0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+(.*)$", next_line
        )
        if not match_next:
            continue
        size = int(match_next.group(1), 16)
        obj = match_next.group(2).strip()
        entries.append((size, name, obj))
    return sorted(entries, reverse=True)


def print_section_summary(
    totals: Dict[str, int],
    ram_size: int,
    flash_size: int,
    device_name: str,
) -> None:
    flash_used = sum(
        totals.get(name, 0)
        for name in (".boot2", ".text", ".rodata", ".binary_info", ".data")
    )
    ram_used = sum(
        totals.get(name, 0)
        for name in (
            ".ram_vector_table",
            ".data",
            ".bss",
            ".heap",
            ".stack1_dummy",
            ".stack_dummy",
        )
    )

    print(f"Device: {device_name}")
    print()
    print("Available memory:")
    print(f"  {'FLASH':16} {format_bytes(flash_size)}")
    print(f"  {'RAM':16} {format_bytes(ram_size)}")
    print()
    print("Flash used:")
    for name in (".boot2", ".text", ".rodata", ".binary_info", ".data"):
        if name in totals:
            print(f"  {name:16} {format_bytes(totals[name])}")
    print(f"  {'TOTAL USED':16} {format_bytes(flash_used)}")
    print()
    remaining_flash = max(flash_size - flash_used, 0)
    print("Flash remaining:")
    print(f"  {format_bytes(remaining_flash)}")
    print()
    print("RAM used:")
    for name in (
        ".ram_vector_table",
        ".data",
        ".bss",
        ".heap",
        ".stack1_dummy",
        ".stack_dummy",
    ):
        if name in totals:
            print(f"  {name:16} {format_bytes(totals[name])}")
    print(f"  {'TOTAL USED':16} {format_bytes(ram_used)}")
    remaining = max(ram_size - ram_used, 0)
    print()
    print("RAM remaining:")
    print(f"  {format_bytes(remaining)}")


def print_top_entries(entries: List[Tuple[int, str, str]], top: int) -> None:
    print()
    print(f"Top {top} .bss/.data entries:")
    for size, name, obj in entries[:top]:
        print(f"  {format_bytes(size):18} {name}  ({obj})")


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize RP2350 memory usage.")
    parser.add_argument(
        "--elf",
        default="cmake-build-debug/PicoUSBKeyBridge.elf",
        help="Path to the ELF file.",
    )
    parser.add_argument(
        "--map",
        default=None,
        help="Path to the ELF map file (defaults to --elf + .map).",
    )
    parser.add_argument(
        "--ram-size",
        type=int,
        default=PICO_RAM_BYTES,
        help="Total RAM size in bytes (default: 520 KB for RP2350).",
    )
    parser.add_argument(
        "--flash-size",
        type=int,
        default=PICO_FLASH_BYTES,
        help="Total flash size in bytes (default: 2 MB for Waveshare RP2350-USB-A).",
    )
    parser.add_argument(
        "--device-name",
        default=PICO_DEVICE_NAME,
        help="Device model name for report header.",
    )
    parser.add_argument(
        "--top", type=int, default=15, help="Number of top RAM entries to show."
    )
    args = parser.parse_args()

    map_path = Path(args.map) if args.map else Path(args.elf).with_suffix(".elf.map")
    if not map_path.exists():
        print(f"Map file not found: {map_path}")
        return 1

    lines = map_path.read_text().splitlines()
    totals = get_section_totals_from_size(Path(args.elf))
    if not totals:
        totals = parse_section_totals(lines)
    entries = parse_bss_data_entries(lines)

    print_section_summary(totals, args.ram_size, args.flash_size, args.device_name)
    print_top_entries(entries, args.top)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
