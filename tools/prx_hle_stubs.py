"""Turn lift_prx.py's <m>_imports.json ({lib: {nid: slot_ea}}) into the lifter's --hle-stubs list
([{library, nid, stub}]). A PRX import slot's initial (relocated) value is the address of its stub
code, the same convention ppu_loader.py uses for an EBOOT.
usage: prx_hle_stubs.py <m>_imports.json <m>_image.bin <base> <out.json>"""
import json, struct, sys

imports_path, image_path, base_s, out_path = sys.argv[1:5]
base = int(base_s, 0)
img = open(image_path, 'rb').read()
def is_stub(addr):
    """An import stub is li r12,0 ... mtctr r0; bctr within its first 8 words."""
    o = addr - base
    if not 0 <= o < len(img) - 0x20:
        return False
    words = [struct.unpack_from('>I', img, o + 4 * i)[0] for i in range(8)]
    return words[0] == 0x39800000 and 0x7C0903A6 in words and 0x4E800420 in words


out, bad = [], []
for lib, nids in json.load(open(imports_path)).items():
    for nid, ea in nids.items():
        ea = int(ea, 0)
        # lift_prx.py may give the stub code address itself, or a slot holding it.
        stub = ea if is_stub(ea) else struct.unpack_from('>I', img, ea - base)[0]
        (out if is_stub(stub) else bad).append({'library': lib, 'nid': nid, 'stub': hex(stub)})
json.dump(out, open(out_path, 'w'), indent=1)
print(f'{len(out)} stubs ok, {len(bad)} rejected -> {out_path}')
for b in bad:
    print('  rejected', b)
