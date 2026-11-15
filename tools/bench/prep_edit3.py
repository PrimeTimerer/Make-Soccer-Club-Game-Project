"""Two (or three) EDIT players, on the bench save copy only."""
from pathlib import Path
import struct
import sys

root = Path(sys.argv[1]).resolve()
count = int(sys.argv[2]) if len(sys.argv) > 2 else 2
assert count in (2, 3)
allowed = Path(__file__).resolve().parents[2] / 'logs' / 'bench'
assert root.is_relative_to(allowed.resolve())
folder = root / 'dev_hdd0/home/00000001/savedata/BLJM61064-SAVEDATA'
save = folder / 'MAIN00.DAT'
d = bytearray(save.read_bytes())
pled = (folder / 'PLED.DAT').read_bytes()
assert struct.unpack_from('>I', pled)[0] == 0x7FFF
master = bytearray(pled[0x218:0x3C0])
extra = bytearray(2 * 0x1A8)
template = pled[:0x218]
slots = list(range(count))
if count == 3:
    # A match fixture needs an already contracted record, not a raw library
    # candidate. Reuse its original slot plus two occupied slots, so the club's
    # roster count and formation references stay valid (no artificial hole).
    own = [i for i in range(23) if struct.unpack_from('>I', d, 0x1B4 + i * 0x218)[0] == 0x7FFF]
    assert len(own) == 1, 'Three-player match fixture requires an existing contracted EDIT player'
    first = own[0]
    template = bytes(d[0x1B4 + first * 0x218:0x1B4 + (first + 1) * 0x218])
    master = bytearray(d[0x8808:0x89B0])
    occupied = [i for i in range(23) if i != first and 0 < struct.unpack_from('>I', d, 0x1B4 + i * 0x218)[0] < 0x7FFB]
    assert len(occupied) >= 2
    slots = [first] + occupied[:2]
# Remove the source save's existing EDIT signing before placing the fixture.
for slot in range(23):
    off = 0x1B4 + slot * 0x218
    if struct.unpack_from('>I', d, off)[0] in (0x7FFF, 0x7FFB, 0x7FFC):
        d[off:off + 0x218] = bytes(0x218)
for index, (slot, pid) in enumerate(zip(slots, [0x7FFF, 0x7FFB, 0x7FFC])):
    rec = bytearray(template)
    struct.pack_into('>I', rec, 0, pid)
    off = 0x1B4 + slot * 0x218
    rec[0x204:0x208] = d[off + 0x204:off + 0x208]
    d[off:off + 0x218] = rec
    m = master.copy()
    struct.pack_into('>H', m, 0, pid)
    m[0x82:0xA6] = ('EDIT ' + chr(65 + index)).encode('utf-16-be').ljust(36, b'\0')
    if index == 0:
        d[0x8808:0x89B0] = m
    else:
        extra[(index - 1) * 0x1A8:index * 0x1A8] = m
if count == 2:
    d[0x1B4 + 2 * 0x218:0x1B4 + 3 * 0x218] = bytes(0x218)
# Clear the native pending signing and current negotiation, in this fixture.
for ea, size in [(0xF6D56C, 0x218), (0xEE6F58, 20), (0xEE6F98, 4)]:
    off = ea - 0xEE27A0
    d[off:off + size] = bytes(size)
save.write_bytes(d)
def digest(data):
    h = 14695981039346656037
    for byte in data:
        h = ((h ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h
sidecar = folder / '.sakatsuku-port' / 'MAIN00.DAT.edit3'
sidecar.parent.mkdir(exist_ok=True)
sidecar.write_bytes(b'SKEDIT3\0' + struct.pack('<QQ', digest(d), digest(extra)) + extra)
print(f'Disposable EDIT fixture: {count} distinct masters; unchanged PLED library')
