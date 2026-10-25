#!/usr/bin/env python3
"""OTA image signing (components/drivers/storage/ota.c trailer): ECDSA P-256 over SHA-256(image || "NDFS" || version || length).
  ota_sign.py keygen KEY.pem            new private key; prints the public key as a C array for ota_set_key()
  ota_sign.py pub KEY.pem               print the public key again
  ota_sign.py sign KEY.pem VERSION IN OUT   append the 76-byte trailer {"NDFS", version, len, r||s}
Keep KEY.pem offline (not in the repository, not in CI logs)."""
import sys, struct, hashlib
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, Prehashed
from cryptography.hazmat.primitives import hashes, serialization

def load(p): return serialization.load_pem_private_key(open(p, 'rb').read(), None)
def pub_bytes(k):
    n = k.public_key().public_numbers(); return n.x.to_bytes(32, 'big') + n.y.to_bytes(32, 'big')
def c_array(b): return 'static const uint8_t ota_pub[64] = {\n' + ''.join(
    '    ' + ', '.join('0x%02x' % x for x in b[i:i + 12]) + ',\n' for i in range(0, 64, 12)) + '};'

cmd = sys.argv[1] if len(sys.argv) > 1 else ''
if cmd == 'keygen':
    k = ec.generate_private_key(ec.SECP256R1())
    open(sys.argv[2], 'wb').write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                                 serialization.NoEncryption()))
    print(c_array(pub_bytes(k)))
elif cmd == 'pub':
    print(c_array(pub_bytes(load(sys.argv[2]))))
elif cmd == 'sign':
    k, ver, img = load(sys.argv[2]), int(sys.argv[3], 0), open(sys.argv[4], 'rb').read()
    head = struct.pack('<III', 0x5346444E, ver, len(img))
    d = hashlib.sha256(img + head).digest()
    r, s = decode_dss_signature(k.sign(d, ec.ECDSA(Prehashed(hashes.SHA256()))))
    open(sys.argv[5], 'wb').write(img + head + r.to_bytes(32, 'big') + s.to_bytes(32, 'big'))
    print('signed %s: %d bytes + trailer, version %d' % (sys.argv[5], len(img), ver))
else:
    sys.exit(__doc__)
