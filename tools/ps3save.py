#!/usr/bin/env python3
"""Convert NFL Head Coach 09 saves between the PS3 (RPCS3) and the port (Xbox 360) formats.

Both consoles are big-endian and write the same data: a save is a small header
block (HED-DATA) and the game's data (USR-DATA, the EA TDB database for rosters,
careers and profiles). The PS3 keeps them as two files in a savedata folder
(BLUS30128-<TYPE>-<NAME>/, plus PARAM.SFO and ICON0.PNG); the 360 keeps one file
with an "MC02" header in front:

    0x00  'MC02'
    0x04  total file size
    0x08  HED-DATA size
    0x0C  USR-DATA size
    0x10  CRC of HED-DATA
    0x14  CRC of USR-DATA
    0x18  CRC of bytes 0x00-0x17
    0x1C  HED-DATA, then USR-DATA

The CRC is EA's (polynomial 0x04C11DB7, MSB first, the first four bytes inverted
as the seed; sub_82F0D220 in default.xex, checked by sub_82D5EED8).

The port keeps saves as loose files: <hc09-cache>/content/<xuid>/00000001/<NAME>/<NAME>
plus <NAME>.name (the display name), where NAME is "<TYPE>-<NAME>" without the
BLUS30128- prefix (the profile is PROFILE-HeadCoach on the 360, PROFILE-HEADCOACH on the PS3).

Inputs are only read. A port save that would be replaced is copied to
<hc09-cache>/content-backup/<timestamp>/ first; an RPCS3 folder is never replaced.

  py -3.11 tools/ps3save.py list   [--ps3 DIR] [--cache DIR]
  py -3.11 tools/ps3save.py import <ps3 save folder>... [--cache DIR] [--force]
  py -3.11 tools/ps3save.py import --all [--ps3 DIR] [--cache DIR] [--force]
  py -3.11 tools/ps3save.py export <port save name or file> <out parent dir> --template <ps3 save folder>
  py -3.11 tools/ps3save.py verify <port save file>...
"""
import argparse
import datetime
import os
import shutil
import struct
import sys

TITLE_ID = 'BLUS30128'
XUID_DIR = 'E000C0AC4E000009'      # runtime/imports_content.cpp kXuid
SAVE_TYPE_DIR = '00000001'         # XCONTENTTYPE_SAVEDGAME
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CACHE = os.path.join(os.path.dirname(HERE), 'hc09-cache')
DEFAULT_PS3 = r'C:\Playstation\dev_hdd0\home\00000001\savedata'

_TABLE = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ 0x04C11DB7) if _c & 0x80000000 else (_c << 1)
        _c &= 0xFFFFFFFF
    _TABLE.append(_c)


def ea_crc(data):
    if len(data) < 4:
        return 0
    c = ~struct.unpack_from('>I', data)[0] & 0xFFFFFFFF
    t = _TABLE
    for b in memoryview(data)[4:]:
        c = t[c >> 24] ^ (((c << 8) & 0xFFFFFFFF) | b)
    return ~c & 0xFFFFFFFF


def pack_mc02(hed, usr):
    head = struct.pack('>4sIIIII', b'MC02', 0x1C + len(hed) + len(usr), len(hed), len(usr),
                       ea_crc(hed), ea_crc(usr))
    return head + struct.pack('>I', ea_crc(head)) + hed + usr


def unpack_mc02(data, check=True):
    magic, total, hs, us, ch, cu, cx = struct.unpack_from('>4sIIIIII', data)
    if magic != b'MC02':
        raise ValueError('not an MC02 save (magic %r)' % magic)
    if total != len(data) or 0x1C + hs + us != total:
        raise ValueError('sizes do not add up: file %d, header says %d = 28 + %d + %d' % (len(data), total, hs, us))
    hed, usr = data[0x1C:0x1C + hs], data[0x1C + hs:]
    if check:
        bad = [n for n, want, got in (('header', cx, ea_crc(data[:0x18])), ('HED-DATA', ch, ea_crc(hed)),
                                      ('USR-DATA', cu, ea_crc(usr))) if want != got]
        if bad:
            raise ValueError('CRC mismatch: ' + ', '.join(bad))
    return hed, usr


# ---- PARAM.SFO (only what an export needs: read, and overwrite string values in place) ----

def sfo_read(data):
    magic, _ver, kt, dt, n = struct.unpack_from('<4sIIII', data)
    if magic != b'\0PSF':
        raise ValueError('not a PARAM.SFO')
    out = {}
    for i in range(n):
        ko, fmt, ln, mx, do = struct.unpack_from('<HHIII', data, 20 + 16 * i)
        key = data[kt + ko:data.index(b'\0', kt + ko)].decode()
        out[key] = (fmt, ln, mx, dt + do, 20 + 16 * i)
    return out


def sfo_set_str(data, key, value):
    ent = sfo_read(bytes(data)).get(key)
    if not ent:
        return
    fmt, _ln, mx, off, idx = ent
    raw = value.encode('utf-8')[:mx - 1] + b'\0'
    data[off:off + mx] = raw.ljust(mx, b'\0')
    struct.pack_into('<I', data, idx + 4, len(raw))


# ---- helpers ----

def ps3_parts(folder):
    """(hed, usr) of a PS3 save folder; follows one level of nesting (a copied folder inside itself)."""
    for d in (folder, os.path.join(folder, os.path.basename(os.path.normpath(folder)))):
        h, u = os.path.join(d, 'HED-DATA'), os.path.join(d, 'USR-DATA')
        if os.path.isfile(h) and os.path.isfile(u):
            with open(h, 'rb') as f:
                hed = f.read()
            with open(u, 'rb') as f:
                usr = f.read()
            return hed, usr
    raise FileNotFoundError('%s has no HED-DATA + USR-DATA (a console copy with PARAM.PFD is still encrypted)' % folder)


PROFILE_NAME = 'PROFILE-HeadCoach'   # the 360 build's one profile save (a constant in default.xex)


def port_name(ps3_folder):
    base = os.path.basename(os.path.normpath(ps3_folder))
    name = base[len(TITLE_ID) + 1:] if base.upper().startswith(TITLE_ID + '-') else base
    return PROFILE_NAME if name.upper() == PROFILE_NAME.upper() else name


def port_dir(cache):
    return os.path.join(cache, 'content', XUID_DIR, SAVE_TYPE_DIR)


def backup(cache, name, stamp):
    src_dir = port_dir(cache)
    dst = os.path.join(cache, 'content-backup', stamp)
    os.makedirs(dst, exist_ok=True)
    if os.path.isdir(os.path.join(src_dir, name)):
        shutil.copytree(os.path.join(src_dir, name), os.path.join(dst, name))
    if os.path.isfile(os.path.join(src_dir, name + '.name')):
        shutil.copy2(os.path.join(src_dir, name + '.name'), dst)
    return dst


def cmd_import(args):
    folders = args.folders
    if args.all:
        folders = [os.path.join(args.ps3, d) for d in sorted(os.listdir(args.ps3)) if d.upper().startswith(TITLE_ID + '-')]
    if not folders:
        sys.exit('nothing to import (give folders or --all)')
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out_root = port_dir(args.cache)
    os.makedirs(out_root, exist_ok=True)
    rc = 0
    for folder in folders:
        name = port_name(folder)
        try:
            hed, usr = ps3_parts(folder)
        except FileNotFoundError as e:
            print('skip   %-28s %s' % (name, e))
            rc = 1
            continue
        target_dir = os.path.join(out_root, name)
        target = os.path.join(target_dir, name)
        if os.path.exists(target_dir):
            if not args.force:
                print('skip   %-28s already in the port (use --force; the old one is backed up first)' % name)
                continue
            print('backup %-28s -> %s' % (name, backup(args.cache, name, stamp)))
        os.makedirs(target_dir, exist_ok=True)
        blob = pack_mc02(hed, usr)
        with open(target + '.tmp', 'wb') as f:
            f.write(blob)
        os.replace(target + '.tmp', target)
        with open(os.path.join(out_root, name + '.name'), 'wb') as f:
            f.write(name.encode('ascii', 'replace'))
        print('import %-28s HED %4d  USR %8d  -> %s' % (name, len(hed), len(usr), target))
    return rc


def resolve_port_file(cache, what):
    if os.path.isfile(what):
        return what
    p = os.path.join(port_dir(cache), what, what)
    if os.path.isfile(p):
        return p
    sys.exit('no port save %r (looked in %s)' % (what, port_dir(cache)))


def cmd_export(args):
    src = resolve_port_file(args.cache, args.save)
    with open(src, 'rb') as f:
        hed, usr = unpack_mc02(f.read())
    name = os.path.basename(src).upper()   # PS3 savedata folder names are upper case
    folder = os.path.join(args.out, '%s-%s' % (TITLE_ID, name))
    if os.path.exists(folder):
        sys.exit('%s exists; not replacing an RPCS3 save (move it away first)' % folder)
    with open(os.path.join(args.template, 'PARAM.SFO'), 'rb') as f:
        sfo = bytearray(f.read())
    sfo_set_str(sfo, 'SAVEDATA_DIRECTORY', '%s-%s' % (TITLE_ID, name))
    sfo_set_str(sfo, 'SAVEDATA_LIST_PARAM', name.split('-', 1)[-1])
    sfo_set_str(sfo, 'SUB_TITLE', name)
    sfo_set_str(sfo, 'DETAIL', 'Converted from the PC port')
    os.makedirs(folder)
    for fn, blob in (('HED-DATA', hed), ('USR-DATA', usr), ('PARAM.SFO', bytes(sfo))):
        with open(os.path.join(folder, fn), 'wb') as f:
            f.write(blob)
    icon = os.path.join(args.template, 'ICON0.PNG')
    if os.path.isfile(icon):
        shutil.copy2(icon, folder)
    print('export %s -> %s' % (src, folder))


def cmd_verify(args):
    rc = 0
    for p in args.files:
        p = resolve_port_file(args.cache, p)
        with open(p, 'rb') as f:
            data = f.read()
        try:
            hed, usr = unpack_mc02(data)
            print('ok     %s  HED %d  USR %d' % (p, len(hed), len(usr)))
        except ValueError as e:
            print('BAD    %s  %s' % (p, e))
            rc = 1
    return rc


def cmd_list(args):
    print('PS3 (%s):' % args.ps3)
    if os.path.isdir(args.ps3):
        for d in sorted(os.listdir(args.ps3)):
            if d.upper().startswith(TITLE_ID + '-'):
                try:
                    hed, usr = ps3_parts(os.path.join(args.ps3, d))
                    print('  %-34s HED %4d  USR %8d' % (d, len(hed), len(usr)))
                except FileNotFoundError:
                    print('  %-34s (no HED-DATA/USR-DATA)' % d)
    print('Port (%s):' % port_dir(args.cache))
    if os.path.isdir(port_dir(args.cache)):
        for d in sorted(os.listdir(port_dir(args.cache))):
            p = os.path.join(port_dir(args.cache), d, d)
            if os.path.isfile(p):
                print('  %-34s %8d bytes' % (d, os.path.getsize(p)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cache', default=DEFAULT_CACHE, help='the port\'s hc09-cache folder (default: %(default)s)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('list')
    p.add_argument('--ps3', default=DEFAULT_PS3)
    p = sub.add_parser('import')
    p.add_argument('folders', nargs='*')
    p.add_argument('--all', action='store_true', help='every BLUS30128-* folder under --ps3')
    p.add_argument('--ps3', default=DEFAULT_PS3)
    p.add_argument('--force', action='store_true', help='replace a port save of the same name (backed up first)')
    p = sub.add_parser('export')
    p.add_argument('save')
    p.add_argument('out', help='the RPCS3 savedata folder (or any folder) to create BLUS30128-<NAME> in')
    p.add_argument('--template', required=True, help='an existing PS3 save folder to take PARAM.SFO and ICON0.PNG from')
    p = sub.add_parser('verify')
    p.add_argument('files', nargs='+')
    a = ap.parse_args()
    return {'import': cmd_import, 'export': cmd_export, 'verify': cmd_verify, 'list': cmd_list}[a.cmd](a) or 0


if __name__ == '__main__':
    sys.exit(main())
