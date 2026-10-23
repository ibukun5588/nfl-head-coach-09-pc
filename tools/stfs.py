"""List an Xbox 360 STFS package (CON/LIVE/PIRS) and extract its files (read-only).

The 1.2 title update is a LIVE package holding one file, default.xexp (docs/HANDOFF_RELEASE.md section 1).
Only files stored in consecutive blocks are extracted (all of a title update's are).

  py -3.11 tools/stfs.py <package> [<output folder>]
"""
import os
import struct
import sys

p = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else None
d = open(p, 'rb').read()
be32 = lambda o: struct.unpack_from('>I', d, o)[0]
le24 = lambda b, o: b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)

print('magic', d[:4], 'size', len(d))
hdr = be32(0x340)
print('header size 0x%X content type 0x%X metadata ver %d' % (hdr, be32(0x344), be32(0x348)))
print('content size 0x%X' % struct.unpack_from('>Q', d, 0x34C)[0])
print('media id %08X  version %08X  base version %08X  title id %08X' % (be32(0x354), be32(0x358), be32(0x35C), be32(0x360)))
print('platform %d exec type %d disc %d/%d  savegame id %08X' % (d[0x364], d[0x365], d[0x366], d[0x367], be32(0x368)))
vd = d[0x379:0x379 + 0x24]
sep, ftcount, ftblock = vd[2], vd[3] | (vd[4] << 8), le24(vd, 5)
print('volume: size 0x%X block separation %d file table blocks %d at block %d, allocated %d' %
      (vd[0], sep, ftcount, ftblock, struct.unpack_from('>I', vd, 0x1C)[0]))
name = d[0x411:0x411 + 0x80].decode('utf-16-be').split('\0')[0]
title = d[0x1691:0x1691 + 0x80].decode('utf-16-be').split('\0')[0]
print('display name %r  title name %r' % (name, title))
first = (hdr + 0xFFF) & ~0xFFF


def offset_of(block, shift):
    # Xenia StfsContainerDevice::BlockToOffsetSTFS: a level-0 hash table before every 0xAA blocks,
    # level-1 before every 0x70E4, level-2 once; each table 1 << shift blocks.
    b = block + (((block // 0xAA) + 1) << shift)
    if block >= 0xAA:
        b += ((block // 0x70E4) + 1) << shift
        if block >= 0x70E4:
            b += 1 << shift
    return first + (b << 12)


def entries(shift):
    raw = b''.join(d[offset_of(ftblock + i, shift):offset_of(ftblock + i, shift) + 0x1000] for i in range(ftcount))
    out = []
    for i in range(0, len(raw), 0x40):
        e = raw[i:i + 0x40]
        if not e[0]:
            break
        n = e[0x28] & 0x3F
        out.append(dict(name=e[:n].decode('latin1'), flags=e[0x28], blocks=le24(e, 0x29), start=le24(e, 0x2F),
                        parent=struct.unpack_from('>h', e, 0x32)[0], size=struct.unpack_from('>I', e, 0x34)[0]))
    return out


for shift in ((0, 1) if sep & 1 else (1, 0)):
    es = entries(shift)
    if es and all(x['name'].isprintable() and x['name'] for x in es):
        break
print('block shift', shift)
for e in es:
    print('  %-40s %s size %9d blocks %5d start %5d parent %d' % (e['name'], 'dir ' if e['flags'] & 0x80 else 'file',
                                                                    e['size'], e['blocks'], e['start'], e['parent']))
if out:
    os.makedirs(out, exist_ok=True)
    for e in es:
        if e['flags'] & 0x80:
            continue
        if not e['flags'] & 0x40:
            print('  ! %s is not stored in consecutive blocks; extraction not implemented' % e['name'])
            continue
        data = b''.join(d[offset_of(e['start'] + i, shift):offset_of(e['start'] + i, shift) + 0x1000] for i in range(e['blocks']))
        open(os.path.join(out, e['name']), 'wb').write(data[:e['size']])
        print('  extracted', e['name'])
