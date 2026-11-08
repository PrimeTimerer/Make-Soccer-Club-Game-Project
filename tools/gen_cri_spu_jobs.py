#!/usr/bin/env python3
"""Wrap the CRI ADX2 SPURS job programs embedded in the EBOOT as SPU ELFs so the
build lifts them (PORT_SPU_INPUTS), and write the table saka_hooks.c registers
them with.

The six programs ("CRI SpursJob <name>/PS3SPU", JOBCRT Ver14 header) are
streamed by the libspurs_jq policy module into the job code buffer at LS
0x7C00 and entered at +0x10. Their CRT relocates itself at start (adding the
load base to the words its table at `ila $r5, <table>` lists): the two header
words +0x3C/+0x40 everywhere, and in AtomDecoder also a codec function-pointer
table. Instructions are never patched, so one lift at base 0x7C00 is exact.
The relocated pointers that land in code are passed as extra function entries.

Outputs (in --out, default spu_jobs/):
  cri_<name>.elf   SPU ELF: base 0x7C00, entry 0x7C10, the initialized image
  cri_jobs.inc     C rows: { name, ea, code_end, image_id, fnv1a64 of code }

Usage: python tools/gen_cri_spu_jobs.py game/EBOOT.ELF [--out spu_jobs]
"""
import argparse, hashlib, os, struct, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "ps3recomp", "tools"))
from wrap_spu_elf import wrap                       # noqa: E402
from spu_elf import scan_spu_candidates             # noqa: E402
from find_spu_functions import detect_functions     # noqa: E402
from spu_disasm import disassemble_spu              # noqa: E402

BASE = 0x7C00
SIG = b"JOBCRT Ver14"


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def fnv1a64(b):
    h = 0xCBF29CE484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def load_segments(elf):
    phoff = struct.unpack_from(">Q", elf, 0x20)[0]
    phsz, phnum = struct.unpack_from(">HH", elf, 0x36)
    segs = []
    for i in range(phnum):
        typ, _flags, off, va, _pa, fsz, _msz, _al = struct.unpack_from(">IIQQQQQQ", elf, phoff + i * phsz)
        if typ == 1:
            segs.append((va, off, fsz))
    return segs


def find_jobs(elf):
    segs = load_segments(elf)
    def f2v(fo):
        for va, off, n in segs:
            if off <= fo < off + n:
                return va + fo - off
        return None
    i = 0
    while True:
        i = elf.find(SIG, i)
        if i < 0:
            return
        fo = i - 0x30
        i += 1
        img_size, x40 = be32(elf, fo + 0x3C), be32(elf, fo + 0x40)
        reloc = None
        for k in range(0x60, 0xA0, 4):               # CRT: ila $r5, <relocation table>
            w = be32(elf, fo + k)
            if (w >> 25) == 0x21 and (w & 0x7F) == 5:
                reloc = (w >> 7) & 0x3FFFF
        blob = elf[fo:fo + x40]
        j = blob.find(b"CRI SpursJob ")
        name = blob[j + 13:j + 60].split(b"/")[0].decode("ascii") if j >= 0 else "job_%X" % fo
        yield dict(name=name, file_off=fo, ea=f2v(fo), img_size=img_size, x40=x40, reloc=reloc, blob=blob)


def relocated_words(blob, reloc):
    out, o = [], reloc
    while True:
        e = be32(blob, o)
        o += 4
        if e == 0:
            return out
        for k in range(4):
            if e & (8 >> k):
                out.append((e & ~0xF) + 4 * k)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("eboot")
    ap.add_argument("--out", default=os.path.join(ROOT, "spu_jobs"))
    args = ap.parse_args()
    elf = open(args.eboot, "rb").read()
    os.makedirs(args.out, exist_ok=True)
    rows = []
    for job in find_jobs(elf):
        blob, reloc = job["blob"], job["reloc"]
        # Code pointers among the relocated words: entries the lifter cannot see.
        extra = sorted({be32(blob, w) for w in relocated_words(blob, reloc)
                        if 0x60 <= be32(blob, w) < reloc and be32(blob, w) % 4 == 0})
        syms = [dict(name="crt_entry", addr=BASE + 0x10, size=0), dict(name="crt_reloc", addr=BASE + 0x60, size=0)]
        syms += [dict(name="fp_%05X" % (BASE + v), addr=BASE + v, size=0) for v in extra]
        # Lift up to the relocation table (code, rodata, data), then cut the
        # image at the first detected function that runs into undecodable words
        # (a constant table after the code: AtomDecoder's float tables). Code
        # left past the cut still runs, interpreted, if anything reaches it.
        limit = reloc
        while True:
            image = blob[:limit]
            wrapped = wrap(image, base=BASE, entry=BASE + 0x10,
                           symbols=[s for s in syms if s["addr"] < BASE + limit])
            funcs, _ = detect_functions(wrapped, verbose=False)
            insns = {i.addr: str(i) for i in disassemble_spu(image[0x60:], BASE + 0x60)}
            bad = [s for s, e in sorted(funcs)
                   if any(".word" in insns.get(a, "") for a in range(max(s, BASE + 0x60), e, 4))]
            if not bad:
                break
            limit = bad[0] - BASE
        path = os.path.join(args.out, "cri_%s.elf" % job["name"].lower())
        with open(path, "wb") as f:
            f.write(wrapped)
        # Same identity the build pipeline derives (spu_program_pipeline.py).
        cands = [c for c in scan_spu_candidates(wrapped) if not c[2]]
        if not cands:
            raise SystemExit("%s: the pipeline does not see the wrapped ELF" % path)
        off, info, _ = cands[0]
        digest = hashlib.sha256(wrapped[off:off + info["size"]]).hexdigest()
        image_id = 0x10000000 | int(digest[:7], 16)
        # Code end: the highest function end the lifter's own analysis finds.
        ends = [e for _s, e in funcs]
        code_end = min(max(ends), BASE + limit) - BASE if ends else limit
        code = blob[:code_end]
        rows.append((job["name"], job["ea"], code_end, image_id, fnv1a64(code), len(funcs), extra))
        print("%-16s ea=0x%06X code 0x%05X..0x%05X (reloc +0x%X) funcs=%d extra=%s image_id=%d" % (
            job["name"], job["ea"], BASE, BASE + code_end, reloc, len(funcs),
            ",".join("0x%X" % (BASE + v) for v in extra) or "-", image_id))
    with open(os.path.join(args.out, "cri_jobs.inc"), "w") as f:
        f.write("/* generated by tools/gen_cri_spu_jobs.py -- do not edit */\n")
        for name, ea, code_end, image_id, fp, nf, _ in rows:
            f.write('{ "%s", 0x%08Xu, 0x%05Xu, %d, 0x%016XULL },  /* %d functions */\n' % (
                name, ea, code_end, image_id, fp, nf))


if __name__ == "__main__":
    main()
