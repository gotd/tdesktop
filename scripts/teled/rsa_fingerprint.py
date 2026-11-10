#!/usr/bin/env python3
"""Print MTProto fingerprints of RSA public keys in a PEM file.

Usage: rsa_fingerprint.py key.pem

Accepts "RSA PUBLIC KEY" (PKCS#1) and "PUBLIC KEY" (SubjectPublicKeyInfo)
blocks. Prints one line per key: "<hex> <signed decimal>", the same value
tdesktop logs as "teled: RSA key fingerprint" and gotd computes with
crypto.RSAFingerprint. Exits with 1 if a private key or no valid public key
is found. Standard library only.
"""
import base64
import hashlib
import re
import struct
import sys

PEM_RE = re.compile(
    r"-----BEGIN ([A-Z ]+)-----\s*(.*?)\s*-----END \1-----", re.DOTALL)


def der_read(data, pos):
    """Return (tag, value, next_pos) of the DER element at pos."""
    tag = data[pos]
    length = data[pos + 1]
    pos += 2
    if length & 0x80:
        count = length & 0x7F
        length = int.from_bytes(data[pos:pos + count], "big")
        pos += count
    value = data[pos:pos + length]
    if len(value) != length:
        raise ValueError("truncated DER")
    return tag, value, pos + length


def parse_pkcs1(der):
    tag, seq, _ = der_read(der, 0)
    if tag != 0x30:
        raise ValueError("expected SEQUENCE")
    tag, n, pos = der_read(seq, 0)
    if tag != 0x02:
        raise ValueError("expected INTEGER n")
    tag, e, _ = der_read(seq, pos)
    if tag != 0x02:
        raise ValueError("expected INTEGER e")
    return n.lstrip(b"\0"), e.lstrip(b"\0")


def parse_spki(der):
    tag, seq, _ = der_read(der, 0)
    if tag != 0x30:
        raise ValueError("expected SEQUENCE")
    tag, algorithm, pos = der_read(seq, 0)
    # rsaEncryption OID 1.2.840.113549.1.1.1
    if tag != 0x30 or b"\x2a\x86\x48\x86\xf7\x0d\x01\x01\x01" not in algorithm:
        raise ValueError("not an RSA key")
    tag, bits, _ = der_read(seq, pos)
    if tag != 0x03 or not bits or bits[0] != 0:
        raise ValueError("expected BIT STRING")
    return parse_pkcs1(bits[1:])


def tl_bytes(value):
    if len(value) < 254:
        out = bytes([len(value)]) + value
    else:
        out = b"\xfe" + struct.pack("<I", len(value))[:3] + value
    return out + b"\0" * (-len(out) % 4)


def fingerprint(n, e):
    digest = hashlib.sha1(tl_bytes(n) + tl_bytes(e)).digest()
    return struct.unpack("<Q", digest[12:20])[0]


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="ascii", errors="replace") as f:
        text = f.read()
    found = 0
    for label, body in PEM_RE.findall(text):
        if "PRIVATE" in label:
            print("error: private key in public key file", file=sys.stderr)
            return 1
        try:
            der = base64.b64decode("".join(body.split()), validate=True)
            if label == "RSA PUBLIC KEY":
                n, e = parse_pkcs1(der)
            elif label == "PUBLIC KEY":
                n, e = parse_spki(der)
            else:
                print(f"skipping '{label}' block", file=sys.stderr)
                continue
        except (ValueError, IndexError) as err:
            print(f"error: bad '{label}' block: {err}", file=sys.stderr)
            return 1
        value = fingerprint(n, e)
        signed = struct.unpack("<q", struct.pack("<Q", value))[0]
        print(f"{value:016x} {signed}")
        found += 1
    if not found:
        print("error: no RSA public key found", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
