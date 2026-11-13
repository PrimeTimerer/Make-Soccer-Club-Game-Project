"""Read Saka Tsuku PARC v2 / SLLZ v1 assets without running the patcher.

The file table uses big-endian 32-byte records; SLLZ declares its own endian.
Offsets and sizes are checked before exposing a payload.
"""
from pathlib import Path
import struct


def decompress(data):
    if data[:4] != b'SLLZ':
        return data
    endian = '>' if data[4] else '<'
    header, size, packed = struct.unpack_from(endian + 'HII', data, 6)
    assert data[5] == 1 and header == 16 and packed <= len(data)
    out = bytearray()
    pos = header + 1
    flag, bits = data[header], 8
    while len(out) < size:
        backref = flag & 0x80
        flag = (flag << 1) & 255
        bits -= 1
        # The decoder prefetches the next flag BEFORE the eighth payload.
        if bits == 0:
            flag, bits = data[pos], 8
            pos += 1
        if backref:
            a, b = data[pos:pos+2]
            pos += 2
            distance, length = ((b << 4) | (a >> 4)) + 1, (a & 15) + 3
            assert distance <= len(out) and len(out) + length <= size
            for _ in range(length):
                out.append(out[-distance])
        else:
            out.append(data[pos])
            pos += 1
    assert pos <= packed
    return bytes(out)


def entries(data):
    assert data[:6] == b'PARC\x02\x01'
    dirs, _, count, table = struct.unpack_from('>4I', data, 16)
    assert table + count * 32 <= len(data)
    for i in range(count):
        name_at = 32 + (dirs + i) * 64
        name = data[name_at:name_at+64].split(b'\0', 1)[0].decode('ascii')
        flags, size, packed, off = struct.unpack_from('>4I', data, table + i * 32)
        assert off + packed <= len(data)
        payload = decompress(data[off:off+packed]) if flags & 0x80000000 else data[off:off+size]
        assert len(payload) == size
        yield name, table + i * 32, off, packed, payload


def compress(data):
    """SLLZ v1; non-overlapping references also work with the game's memcpy."""
    out = bytearray(b'\0')
    flag_at, bits, pos = 0, 8, 0
    while pos < len(data):
        distance = length = 0
        for n in range(min(18, len(data) - pos), 2, -1):
            at = data.rfind(data[pos:pos+n], max(0, pos - 4096), pos)
            if at >= 0:
                distance, length = pos - at, n
                break
        if length:
            out[flag_at] |= 1 << (bits - 1)
            value = distance - 1
            payload = bytes((((value & 15) << 4) | (length - 3), value >> 4))
        else:
            length, payload = 1, data[pos:pos+1]
        bits -= 1
        if bits == 0:
            flag_at, bits = len(out), 8
            out.append(0)
        out += payload
        pos += length
    result = b'SLLZ\0\1\x10\0' + struct.pack('<II', len(data), len(out) + 16) + out
    assert decompress(result) == data
    return result


def replace_payloads(data, replacements):
    """Repack PAR members at their original offsets, within each allocation."""
    out = bytearray(data)
    rows = list(entries(data))
    for name, table, off, packed, payload in rows:
        if name not in replacements:
            continue
        new = replacements[name]
        assert len(new) == len(payload)
        capacity = min([p for _, _, p, _, _ in rows if p > off] + [len(data)]) - off
        encoded = compress(new)
        assert len(encoded) <= capacity, f'{name}: packed {len(encoded)} > capacity {capacity}'
        out[off:off+capacity] = encoded + bytes(capacity-len(encoded))
        struct.pack_into('>I', out, table+8, len(encoded))
    for name, _, _, _, payload in entries(out):
        old = next(v for n, _, _, _, v in rows if n == name)
        assert payload == replacements.get(name, old)
    return bytes(out)


if __name__ == '__main__':
    import sys
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for name, _, _, _, payload in entries(Path(sys.argv[1]).read_bytes()):
        assert Path(name).name == name
        (out / name).write_bytes(payload)
        print(name, len(payload))
