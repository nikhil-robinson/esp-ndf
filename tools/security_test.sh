#!/bin/bash
# Host security regression suite: every parser fuzzer with ASan + UBSan (no recovery: any finding fails the run),
# the OTA signature cross-check and the PFS power-cut test. Usage: tools/security_test.sh [rounds-scale] (default 1)
set -e
cd "$(dirname "$0")/.."
K=${1:-1}; O=build/security; mkdir -p $O
F="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Itools/fuzz/host -Icomponents/include -Icomponents/soc/esp32s3"
gcc $F tools/fuzz/fuzz_fat.c components/drivers/storage/fat.c -o $O/fat
gcc $F tools/fuzz/fuzz_flashfs.c components/drivers/storage/spiffs.c components/drivers/storage/nvs.c components/lib/crc32.c -o $O/flashfs
gcc $F tools/fuzz/fuzz_wl.c components/drivers/storage/wl.c components/lib/crc32.c -o $O/wl
gcc $F tools/fuzz/fuzz_msc.c components/drivers/usb/usb_msc.c -o $O/msc
gcc $F tools/test_pfs.c components/drivers/storage/pfs.c components/lib/crc32.c -o $O/pfs
gcc $F tools/fuzz/test_ota_sig.c components/lib/ecc.c components/lib/p256_comb.c -o $O/sig
$O/fat $((20000 * K)) 7
$O/flashfs spiffs $((10000 * K)) 7
$O/flashfs nvs $((10000 * K)) 7
$O/wl $((20000 * K)) 7
$O/msc $((300000 * K)) 7
$O/pfs $((20000 * K)) 7
python3 - $O <<'PY'
import sys, hashlib, struct, subprocess, os
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, Prehashed
from cryptography.hazmat.primitives import hashes
k = ec.generate_private_key(ec.SECP256R1()); n = k.public_key().public_numbers()
pub = n.x.to_bytes(32, 'big') + n.y.to_bytes(32, 'big'); d = hashlib.sha256(os.urandom(500)).digest()
r, s = decode_dss_signature(k.sign(d, ec.ECDSA(Prehashed(hashes.SHA256()))))
out = subprocess.run([sys.argv[1] + '/sig', pub.hex(), d.hex(), (r.to_bytes(32, 'big') + s.to_bytes(32, 'big')).hex()], capture_output=True, text=True)
print(out.stdout.strip()); sys.exit(out.returncode)
PY
echo "=== security-test ALL PASS ==="
