#!/usr/bin/env python3
# =============================================================================
# tools/get_toolchain.py  –  Download the Xtensa ESP32-S3 GCC toolchain
#
# Downloads the prebuilt xtensa-esp32s3-elf toolchain from Espressif's
# GitHub releases page and extracts it to ./tools/xtensa-esp32s3-elf/.
#
# Usage:
#   python tools/get_toolchain.py
#   python tools/get_toolchain.py --version 13.2.0_20240530 --prefix ./toolchains
#
# After download, add the bin directory to your PATH:
#   export PATH=$(pwd)/tools/xtensa-esp32s3-elf/bin:$PATH
#   (Windows PowerShell: $env:PATH = "$(Get-Location)\tools\xtensa-esp32s3-elf\bin;$env:PATH")
# =============================================================================

import argparse
import os
import platform
import ssl
import sys
import tarfile
import urllib.request

# Verify lzma is available (sometimes absent on Windows Python installs)
try:
    import lzma  # noqa: F401  – needed by tarfile 'r:xz'
except ImportError:
    print('[get_toolchain] ERROR: Python lzma module is missing.')
    print('  On Windows, reinstall Python from python.org and tick')
    print('  "Optional Features" → "pip" and ensure no modules are excluded.')
    print('  Alternatively install python via: winget install Python.Python.3.12')
    sys.exit(1)

# ---- Release configuration ------------------------------------------------ #
# Check https://github.com/espressif/crosstool-NG/releases for latest
DEFAULT_VERSION = '13.2.0_20240530'
BASE_URL        = 'https://github.com/espressif/crosstool-NG/releases/download'

PLATFORM_MAP = {
    ('Linux',   'x86_64'):  'x86_64-linux-gnu',
    ('Darwin',  'x86_64'):  'x86_64-apple-darwin',
    ('Darwin',  'arm64'):   'aarch64-apple-darwin',
    ('Windows', 'AMD64'):   'x86_64-w64-mingw32',
    ('Windows', 'x86_64'):  'x86_64-w64-mingw32',
}


def get_platform_suffix() -> str:
    system  = platform.system()
    machine = platform.machine()
    key = (system, machine)
    if key not in PLATFORM_MAP:
        print(f'[get_toolchain] Unsupported platform: {system} / {machine}')
        print(f'  Supported: {list(PLATFORM_MAP.keys())}')
        sys.exit(1)
    return PLATFORM_MAP[key]


def _ssl_context() -> ssl.SSLContext:
    """Return an SSL context, falling back to unverified if cert store is missing."""
    try:
        ctx = ssl.create_default_context()
        # Smoke-test: try to load the default CA bundle
        ctx.load_default_certs()
        return ctx
    except Exception:
        pass
    # Try certifi if installed
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        pass
    print('[get_toolchain] WARNING: SSL certificate verification disabled.')
    print('  Run: pip install certifi  to fix this properly.')
    return ssl._create_unverified_context()  # type: ignore[attr-defined]


def download(url: str, dest: str) -> None:
    print(f'[get_toolchain] Downloading {url}')
    try:
        req = urllib.request.Request(url, headers={'User-Agent': 'get_toolchain/1.0'})
        with urllib.request.urlopen(req, context=_ssl_context()) as response:
            total = int(response.headers.get('Content-Length', 0))
            downloaded = 0
            chunk_size = 65536
            with open(dest, 'wb') as f:
                while True:
                    chunk = response.read(chunk_size)
                    if not chunk:
                        break
                    f.write(chunk)
                    downloaded += len(chunk)
                    if total:
                        pct = 100 * downloaded // total
                        print(f'\r  {pct:3d}% ({downloaded // 1024 // 1024} MB)', end='', flush=True)
        print()
    except Exception as e:
        print(f'\n[get_toolchain] Download failed: {e}')
        sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser(
        description='Download Xtensa ESP32-S3 GCC toolchain')
    # Default prefix: <repo-root>/tools/ — resolved relative to this script's
    # directory so it works regardless of where the user runs the command from.
    script_dir  = os.path.dirname(os.path.abspath(__file__))
    default_prefix = script_dir  # script lives in tools/, so extract there

    parser.add_argument('--version', default=DEFAULT_VERSION,
                        help=f'Toolchain version (default: {DEFAULT_VERSION})')
    parser.add_argument('--prefix',  default=default_prefix,
                        help='Directory to extract toolchain into (default: <repo>/tools/)')
    parser.add_argument('--force',   action='store_true',
                        help='Re-download even if toolchain already exists')
    args = parser.parse_args()

    suffix   = get_platform_suffix()
    archive  = f'xtensa-esp32s3-elf-{args.version}-{suffix}.tar.xz'
    url      = f'{BASE_URL}/esp-{args.version}/{archive}'
    dest_dir = os.path.abspath(args.prefix)
    tc_dir   = os.path.join(dest_dir, 'xtensa-esp32s3-elf')
    tc_bin   = os.path.join(tc_dir, 'bin')

    if os.path.isdir(tc_bin) and not args.force:
        print(f'[get_toolchain] Toolchain already exists at {tc_dir}')
        print(f'  Add to PATH: {tc_bin}')
        return

    os.makedirs(dest_dir, exist_ok=True)
    archive_path = os.path.join(dest_dir, archive)

    download(url, archive_path)

    print(f'[get_toolchain] Extracting to {dest_dir}/')
    with tarfile.open(archive_path, 'r:xz') as tar:
        # filter='data' strips dangerous paths (uid/gid/devices) – required on
        # Python 3.12+ to suppress DeprecationWarning; safe on all versions.
        extract_kwargs = {}
        if sys.version_info >= (3, 12):
            extract_kwargs['filter'] = 'data'
        tar.extractall(dest_dir, **extract_kwargs)

    os.remove(archive_path)
    print(f'[get_toolchain] Toolchain installed at: {tc_dir}')
    print()

    if platform.system() == 'Windows':
        print(f'  PowerShell: $env:PATH = "{tc_bin};$env:PATH"')
        print(f'  Or set XTENSA_TOOLCHAIN_PATH={os.path.dirname(tc_bin)}')
    else:
        print(f'  Bash: export PATH="{tc_bin}:$PATH"')
        print(f'  Or: export XTENSA_TOOLCHAIN_PATH="{os.path.dirname(tc_bin)}"')

    print()
    print('[get_toolchain] Verify with: xtensa-esp32s3-elf-gcc --version')


if __name__ == '__main__':
    main()
