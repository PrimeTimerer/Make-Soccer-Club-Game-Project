"""Reader for the game's text archives (data/text*/*.arch: little-endian, UTF-16LE strings).

Layout: u32 table count N, u32 size, u32 4, u32 1, N x u32 table offsets (from the end of that list). A table: u16 kind,
u16 entry count, count x (u32 id, u32 string offset), then the strings (from the end of the entry list), each a u16
byte length (with the terminating 0) and UTF-16LE text. txt_static_data.arch table 0 holds the player names by player
ID (0x46A2 entries, the game's player ID range).
usage: saka_text.py <file.arch> [table [id ...]]   (no table: one line per table with its first strings)"""
import struct
import sys


def tables(path):
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 0)[0]
    base = 0x10 + 4 * n
    out = []
    for k in range(n):
        t = base + struct.unpack_from("<I", d, 0x10 + 4 * k)[0]
        kind, count = struct.unpack_from("<HH", d, t)
        strings = t + 4 + 8 * count
        entries = {}
        for e in range(count):
            i, off = struct.unpack_from("<II", d, t + 4 + 8 * e)
            p = strings + off
            ln = struct.unpack_from("<H", d, p)[0]
            entries[i] = d[p + 2:p + ln].decode("utf-16-le", "replace").rstrip("\0")
        out.append((kind, entries))
    return out


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    ts = tables(sys.argv[1])
    if len(sys.argv) == 2:
        for k, (kind, e) in enumerate(ts):
            ids = sorted(e)
            print(k, "kind %d" % kind, "%d entries" % len(e), " | ".join("%d:%s" % (i, e[i][:20]) for i in ids[1:6]))
    else:
        kind, e = ts[int(sys.argv[2])]
        for a in sys.argv[3:] or [str(i) for i in sorted(e)]:
            i = int(a, 0)
            print(i, hex(i), e.get(i))
