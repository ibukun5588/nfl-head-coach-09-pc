#!/usr/bin/env python3
"""Read guest memory of the running hc09.exe (Windows, read-only).

  py -3.11 tools/gmem.py <log> <guest address hex> <bytes>

<log> is that run's stderr log: its line "[runtime] guest memory @ host <address>" gives where the
4 GiB guest space is mapped. Prints big-endian words and ASCII, 16 bytes per row. Used to read live
game state (e.g. the SuperSim sides after a simmed game) without stopping the game.
"""
import ctypes, ctypes.wintypes as w, re, struct, subprocess, sys


def host_base(log):
    data = open(log, 'rb').read().replace(b'\0', b'')
    m = re.search(rb'guest memory @ host ([0-9A-Fa-f]+)', data)
    if not m:
        sys.exit('no "guest memory @ host" line in ' + log)
    return int(m.group(1), 16)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    base, ga, n = host_base(sys.argv[1]), int(sys.argv[2], 16), int(sys.argv[3])
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq hc09.exe', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    if 'hc09.exe' not in out:
        sys.exit('hc09.exe is not running')
    pid = int(out.split(',')[1].strip('"'))
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.restype = w.HANDLE
    h = k.OpenProcess(0x0010 | 0x0400, False, pid)   # PROCESS_VM_READ | PROCESS_QUERY_INFORMATION
    buf, got = ctypes.create_string_buffer(n), ctypes.c_size_t()
    if not k.ReadProcessMemory(h, ctypes.c_void_p(base + ga), buf, n, ctypes.byref(got)):
        sys.exit('ReadProcessMemory failed: %d' % ctypes.get_last_error())
    d = buf.raw
    for i in range(0, n, 16):
        row = d[i:i + 16]
        words = struct.unpack('>4I', row.ljust(16, b'\0'))
        print('%08X  %s  %s' % (ga + i, ' '.join('%08X' % x for x in words), ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)))


if __name__ == '__main__':
    main()
