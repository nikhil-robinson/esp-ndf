#!/usr/bin/env python3
"""Test PKI for the TLS tests: a CA and a server certificate (ECDSA P-256 / SHA-256, SAN ndf.local and *.example.test), plus a second CA that signs nothing we trust.
usage: tls_certs.py <outdir>   writes ca.pem ca_pub.hex server.der server.pem server_key.pem server_priv.hex other_ca_pub.hex"""
import datetime, os, sys
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
def name(cn): return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])
def pub_hex(k):
    n = k.public_key().public_numbers(); return (n.x.to_bytes(32, 'big') + n.y.to_bytes(32, 'big')).hex()
def ca(cn):
    k = ec.generate_private_key(ec.SECP256R1()); now = datetime.datetime(2026, 1, 1)
    c = (x509.CertificateBuilder().subject_name(name(cn)).issuer_name(name(cn)).public_key(k.public_key()).serial_number(x509.random_serial_number())
         .not_valid_before(now).not_valid_after(now + datetime.timedelta(days=3650)).add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True).sign(k, hashes.SHA256()))
    return k, c
cak, cac = ca('ndf test CA'); ok, oc = ca('other CA')
sk = ec.generate_private_key(ec.SECP256R1()); now = datetime.datetime(2026, 1, 1)
sc = (x509.CertificateBuilder().subject_name(name('ndf.local')).issuer_name(cac.subject).public_key(sk.public_key()).serial_number(x509.random_serial_number())
      .not_valid_before(now).not_valid_after(now + datetime.timedelta(days=3650))
      .add_extension(x509.SubjectAlternativeName([x509.DNSName('ndf.local'), x509.DNSName('*.example.test')]), critical=False)
      .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True).sign(cak, hashes.SHA256()))
w = lambda f, b, m='wb': open(os.path.join(out, f), m).write(b)
w('ca.pem', cac.public_bytes(serialization.Encoding.PEM)); w('other_ca.pem', oc.public_bytes(serialization.Encoding.PEM)); w('ca_pub.hex', pub_hex(cak), 'w'); w('other_ca_pub.hex', pub_hex(ok), 'w')
w('server.der', sc.public_bytes(serialization.Encoding.DER)); w('server.pem', sc.public_bytes(serialization.Encoding.PEM))
w('server_key.pem', sk.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
w('server_priv.hex', sk.private_numbers().private_value.to_bytes(32, 'big').hex(), 'w'); w('server_pub.hex', pub_hex(sk), 'w')
print('certificates in', out)
