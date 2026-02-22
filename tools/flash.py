#!/usr/bin/env python3
# =============================================================================
# tools/flash.py  –  Flash helper for esp-ndf bare-metal builds
#
# Wraps esptool.py with sane defaults for ESP32-S3 bare-metal projects.
# Builds the partition table on the fly and flashes bootloader + partition
# table + app in one go.
#
# Prerequisites:
#   pip install esptool
#
# Usage:
#   python tools/flash.py --port COM3 --baud 921600 build/examples/blink/blink.elf
#
#   Options:
#     --port    Serial port (default: auto-detect)
#     --baud    Upload baud rate (default: 460800)
#     --flash-size  8MB | 4MB | 16MB (default: 8MB)
#     --flash-mode  qio | qout | dio | dout (default: qio)
#     --flash-freq  80m | 40m (default: 80m)
#     --no-stub     Disable ROM stub upload (slower but more compatible)
#     --erase-all   Erase entire flash before flashing
#     --monitor     Open serial monitor after flashing
#     --baud-monitor  Monitor baud rate (default: 115200)
# =============================================================================

import argparse
import os
import struct
import subprocess
import sys
import tempfile

# ---- Default flash layout ------------------------------------------------ #
BOOTLOADER_OFFSET  = 0x0000   # We use built-in ROM bootloader (no custom bootloader)
PARTITION_OFFSET   = 0x8000   # Partition table at 32 KB
APP_OFFSET         = 0x10000  # App at 64 KB (first partition)

# ---- Minimal partition table entry --------------------------------------- #
# Format: magic(2) type(1) subtype(1) offset(4) size(4) name(16) flags(4)
PARTITION_MAGIC = 0xAA50

def make_partition_table(app_size_mb: int = 4) -> bytes:
    """Generate a minimal single-app partition table (no OTA)."""

    def entry(name: str, ptype: int, subtype: int, offset: int, size: int) -> bytes:
        name_bytes = name.encode('utf-8', errors='replace')[:15].ljust(16, b'\x00')
        return struct.pack('<HBBI16sI',
                          PARTITION_MAGIC,
                          ptype, subtype,
                          offset, size,
                          name_bytes,
                          0x0)  # flags

    # Factory app partition (type=0 APP, subtype=0 FACTORY)
    app_entry = entry('app', 0x00, 0x00, APP_OFFSET, app_size_mb * 1024 * 1024)

    # NVS partition (type=1 DATA, subtype=2 NVS)
    nvs_start  = APP_OFFSET + app_size_mb * 1024 * 1024
    nvs_entry  = entry('nvs', 0x01, 0x02, nvs_start, 0x6000)

    table = app_entry + nvs_entry
    # Pad to 0xC00 bytes (3 KB, standard partition table size)
    table = table.ljust(0xC00, b'\xFF')
    # ESP32 partition table checksum (MD5 – esptool handles the final binary)
    return table


def find_esptool() -> str:
    """Return path to esptool.py or esptool executable."""
    for candidate in ('esptool.py', 'esptool'):
        try:
            subprocess.check_output([candidate, 'version'],
                                    stderr=subprocess.DEVNULL)
            return candidate
        except (FileNotFoundError, subprocess.CalledProcessError):
            pass

    # Try the Python-module path
    try:
        import esptool  # noqa: F401
        return sys.executable + ' -m esptool'
    except ImportError:
        pass

    print('[flash] ERROR: esptool not found.')
    print('        Install it with: pip install esptool')
    sys.exit(1)


def elf_to_bin(esptool: str, elf_path: str, out_path: str,
               flash_mode: str, flash_freq: str, flash_size: str) -> None:
    """Convert ELF to ESP32 image binary using esptool elf2image."""
    cmd = [
        *esptool.split(),
        '--chip', 'esp32s3',
        'elf2image',
        '--flash_mode', flash_mode,
        '--flash_freq', flash_freq,
        '--flash_size', flash_size,
        '-o', out_path,
        elf_path,
    ]
    print(f'[flash] Converting ELF: {" ".join(cmd)}')
    subprocess.check_call(cmd)


def write_flash(esptool: str, port: str, baud: int,
                flash_mode: str, flash_freq: str, flash_size: str,
                bin_path: str, part_table_path: str,
                no_stub: bool, erase_all: bool) -> None:
    """Flash partition table + app binary to the device."""

    cmd = [
        *esptool.split(),
        '--chip', 'esp32s3',
        '--port', port,
        '--baud', str(baud),
    ]
    if no_stub:
        cmd.append('--no-stub')

    cmd += ['write_flash']

    if erase_all:
        cmd.append('--erase-all')

    cmd += [
        '--flash_mode', flash_mode,
        '--flash_freq', flash_freq,
        '--flash_size', flash_size,
        # Partition table first
        str(PARTITION_OFFSET), part_table_path,
        # Then the app image
        str(APP_OFFSET), bin_path,
    ]

    print(f'[flash] Flashing: {" ".join(cmd)}')
    subprocess.check_call(cmd)


def run_monitor(port: str, baud: int) -> None:
    """Open a serial monitor (reads until Ctrl-C)."""
    print(f'\n[monitor] Opened {port} at {baud} baud. Press Ctrl-C to exit.\n')
    try:
        import serial
    except ImportError:
        print('[monitor] Install pyserial: pip install pyserial')
        return

    with serial.Serial(port, baud, timeout=0.1) as ser:
        try:
            while True:
                data = ser.read(256)
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
        except KeyboardInterrupt:
            pass


# ============================================================================
# main
# ============================================================================

def main() -> None:
    parser = argparse.ArgumentParser(
        description='flash.py – esp-ndf bare-metal flash helper')
    parser.add_argument('elf',             help='Path to the application ELF file')
    parser.add_argument('--port',          default='auto',  help='Serial port')
    parser.add_argument('--baud',          type=int, default=460800)
    parser.add_argument('--flash-size',    default='8MB')
    parser.add_argument('--flash-mode',    default='qio')
    parser.add_argument('--flash-freq',    default='80m')
    parser.add_argument('--no-stub',       action='store_true')
    parser.add_argument('--erase-all',     action='store_true')
    parser.add_argument('--monitor',       action='store_true')
    parser.add_argument('--baud-monitor',  type=int, default=115200)
    args = parser.parse_args()

    if not os.path.exists(args.elf):
        print(f'[flash] ERROR: ELF file not found: {args.elf}')
        sys.exit(1)

    esptool_cmd = find_esptool()

    with tempfile.TemporaryDirectory() as tmpdir:
        # 1. Convert ELF → image binary
        bin_path = os.path.join(tmpdir, 'app.bin')
        elf_to_bin(esptool_cmd, args.elf, bin_path,
                   args.flash_mode, args.flash_freq, args.flash_size)

        # 2. Generate partition table
        part_path = os.path.join(tmpdir, 'partitions.bin')
        with open(part_path, 'wb') as f:
            f.write(make_partition_table())
        print(f'[flash] Partition table written to {part_path}')

        # 3. Flash
        port = args.port
        if port == 'auto':
            # Let esptool auto-detect
            port = '/dev/ttyUSB0' if sys.platform != 'win32' else 'COM1'
            print(f'[flash] WARNING: port=auto, guessing {port}.')
            print(f'         Use --port COMX or --port /dev/ttyUSBx to specify.')

        write_flash(esptool_cmd, port, args.baud,
                    args.flash_mode, args.flash_freq, args.flash_size,
                    bin_path, part_path,
                    args.no_stub, args.erase_all)

    print('\n[flash] Done! Reset the board if it does not reboot automatically.')

    if args.monitor:
        run_monitor(port, args.baud_monitor)


if __name__ == '__main__':
    main()
