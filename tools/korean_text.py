"""The v0.8 font's KS X 1001 Hangul -> Shift-JIS CJK glyph mapping."""
from pathlib import Path
import struct


def glyph_map():
    hangul = [bytes((a, b)).decode('euc_kr')
              for a in range(0xB0, 0xC9) for b in range(0xA1, 0xFF)]
    encoded = []
    for code in range(0x889F, 0x96FD):
        # Font atlas 9394 remains Japanese in the supplied patch.
        if code >> 8 in (0x93, 0x94):
            continue
        try:
            char = code.to_bytes(2, 'big').decode('cp932')
        except UnicodeDecodeError:
            continue
        if len(char) == 1 and '\u4e00' <= char <= '\u9fff':
            encoded.append(char)
    assert len(encoded) >= len(hangul)
    return dict(zip(hangul, encoded))


ENCODE = glyph_map()
DECODE = {v: k for k, v in ENCODE.items()}


def decode(text):
    return ''.join(DECODE.get(c, c) for c in text)


def encode(text):
    for c in text:
        if '\uac00' <= c <= '\ud7a3' and c not in ENCODE:
            raise ValueError('Font has no glyph for ' + c)
    return ''.join(ENCODE.get(c, c) for c in text)


def tables(data, standalone=False):
    count = 1 if standalone else struct.unpack_from('<I', data)[0]
    base = 16 + 4 * count
    offsets = [0] if standalone else [base + struct.unpack_from('<I', data, 16 + i * 4)[0] for i in range(count)]
    for table, at in enumerate(offsets):
        kind, n = struct.unpack_from('<HH', data, at)
        strings = at + 4 + n * 8
        for i in range(n):
            key, off = struct.unpack_from('<II', data, at + 4 + i * 8)
            pos = strings + off
            size = struct.unpack_from('<H', data, pos)[0]
            text = data[pos+2:pos+size].decode('utf-16-le').rstrip('\0')
            yield table, key, pos, size, text


def repack(data, replacements, standalone=False):
    """Rebuild offsets; IDs/order/kinds and distinct string records stay intact.

    Preserve the native layout: separate records, an extra zero u16 after the
    terminated text, then four-byte alignment. Offset sharing and minimal
    padding are deliberately avoided; those variants failed the runtime load.
    """
    count = 1 if standalone else struct.unpack_from('<I', data)[0]
    base = 16 + count * 4
    starts = [0] if standalone else [base + struct.unpack_from('<I', data, 16 + i * 4)[0] for i in range(count)]
    grouped = [[] for _ in starts]
    for table, key, _, _, text in tables(data, standalone):
        grouped[table].append((key, replacements.get((table, key), text)))
    out = bytearray() if standalone else bytearray(data[:base])
    for table, rows in enumerate(grouped):
        if not standalone:
            struct.pack_into('<I', out, 16 + table * 4, len(out) - base)
        kind = struct.unpack_from('<H', data, starts[table])[0]
        block = bytearray(struct.pack('<HH', kind, len(rows)))
        strings = bytearray()
        for key, text in rows:
            offset = len(strings)
            raw = text.encode('utf-16-le')
            strings += struct.pack('<H', len(raw) + 2) + raw + b'\0\0\0\0'
            strings += bytes((-len(strings)) % 4)
            block += struct.pack('<II', key, offset)
        out += block + strings
    if not standalone:
        struct.pack_into('<I', out, 4, len(out) - base)
    out += bytes(max(0, len(data) - len(out)))
    expected = [(t, k, replacements.get((t, k), s)) for t, k, _, _, s in tables(data, standalone)]
    assert [(t, k, s) for t, k, _, _, s in tables(out, standalone)] == expected
    return bytes(out)


if __name__ == '__main__':
    import sys
    path = Path(sys.argv[1])
    for table, key, _, _, text in tables(path.read_bytes(), path.suffix == '.txb'):
        print(table, key, decode(text).replace('\n', '\\n'), sep='\t')
