#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixtures for limporttest.c: a config.conf as M5Launcher writes it, and
a stand-in for its .bin with the key somewhere in it. The encryption is
the `cryptography` package's, not limport_core.c's, so the test does not
check the code against itself.

    gen_fixture.py <outdir> <key> <key_offset>
"""
import base64, json, os, random, sys
from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

out, keystr, off = sys.argv[1], sys.argv[2], int(sys.argv[3])
raw = keystr.encode()
key = bytes(raw[i % len(raw)] for i in range(16))     # cycled / cut to 16
IV = b"LauncherWifiKey!"

def enc(pw: str) -> str:
    p = padding.PKCS7(128).padder()
    data = p.update(pw.encode()) + p.finalize()
    e = Cipher(algorithms.AES(key), modes.CBC(IV)).encryptor()
    return base64.b64encode(e.update(data) + e.finalize()).decode()

nets = [
    ("Home",              "correct horse battery"),
    ("Caf\u00e9 \u2615",  "p4ssw0rd-with-a-longer-tail-1234567890"),   # escaped in the JSON
    ("Phone",             "12345678"),                                  # exactly 16 bytes padded
    ("Psk",               "0123456789abcdef" * 4),                      # a 64-hex PSK
    ("Max",               "x" * 63),
]
wifi = [{"ssid": s, "pwd": enc(p), "secure": True} for s, p in nets]
wifi.insert(2, {"ssid": "Open", "pwd": "", "secure": False})
wifi.append({"ssid": "x" * 33, "pwd": enc("toolongssid"), "secure": True})   # SSID over 32 bytes
# M5Launcher's file: an array of objects, wifi is in one; others come first.
doc = [{"theme": {"a": [1, 2, {"wifi": "not an array"}]}}, {"wifi": wifi}, {"other": 1}]

os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "config.conf"), "w") as f:
    json.dump(doc, f, indent=2)          # ensure_ascii: \u00e9, and a surrogate pair below
with open(os.path.join(out, "config_min.conf"), "w") as f:
    json.dump(doc, f, separators=(",", ":"), ensure_ascii=False)

random.seed(5)
blob = bytearray(random.randrange(128, 256) for _ in range(20000))   # no printable noise
blob[3000:3000 + len(IV)] = IV                                        # the marker
blob[500:520] = b"some decoy string...."
blob[off - 1] = 0
blob[off:off + len(raw)] = raw
blob[off + len(raw)] = 0
open(os.path.join(out, "launcher.bin"), "wb").write(blob)
bad = bytearray(blob)
bad[off:off + len(raw)] = bytes(random.randrange(128, 256) for _ in raw)   # key gone
open(os.path.join(out, "nokey.bin"), "wb").write(bad)

with open(os.path.join(out, "expect.txt"), "w") as f:
    for s, p in nets:
        f.write(f"{s}\t{p}\n")
    f.write("KEY\t" + key.hex() + "\n")
