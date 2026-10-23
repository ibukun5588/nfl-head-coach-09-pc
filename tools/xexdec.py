#!/usr/bin/env python3
"""Decrypt and de-block a retail XEX2 (BASIC compression) into its PE image.

Usage: python3 xexdec.py default.xex headcoach.basefile
Requires: pip install cryptography
"""
import struct, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")

def main(inp, outp):
    data = open(inp, 'rb').read()
    u32 = lambda o: struct.unpack('>I', data[o:o+4])[0]
    assert data[:4] == b'XEX2', "not a XEX2 file"
    pe_data_off, sec = u32(0x08), u32(0x10)
    opt = {}
    for i in range(u32(0x14)):
        off = 0x18 + i*8
        opt[u32(off)] = u32(off+4)
    image_size = u32(sec + 0x04)
    enc_title_key = data[sec+0x150 : sec+0x150+16]
    d = Cipher(algorithms.AES(RETAIL_KEY), modes.ECB()).decryptor()
    session_key = d.update(enc_title_key) + d.finalize()
    ff = opt[0x000003FF]
    enc_type, comp_type = struct.unpack('>HH', data[ff+4:ff+8])
    if enc_type == 0 and comp_type == 0:
        # Raw image: what XexPatcher writes for a title-updated xex (decrypted, uncompressed).
        out = data[pe_data_off:pe_data_off + image_size]
        assert len(out) == image_size and out[:2] == b'MZ', "raw image incomplete"
        open(outp, 'wb').write(out)
        print(f"OK: {len(out)} bytes (raw, not encrypted)")
        return
    assert comp_type == 1, "only BASIC compression supported"
    blocks, p = [], ff + 8
    for _ in range((u32(ff) - 8) // 8):
        blocks.append((u32(p), u32(p+4))); p += 8
    blob = data[pe_data_off:]
    d = Cipher(algorithms.AES(session_key), modes.CBC(b'\x00'*16)).decryptor()
    dec = d.update(blob[:(len(blob)//16)*16]) + d.finalize()
    out, src = bytearray(), 0
    for ds, zs in blocks:
        out += dec[src:src+ds]; out += b'\x00'*zs; src += ds
    assert len(out) == image_size and out[:2] == b'MZ', "reconstruction failed"
    open(outp, 'wb').write(out)
    print(f"OK: {len(out)} bytes, session key {session_key.hex()}")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
