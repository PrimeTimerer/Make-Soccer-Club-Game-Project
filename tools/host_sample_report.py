#!/usr/bin/env python3
"""Resolve a SAKA_HOST_SAMPLE file (rva count lines) against the lld-link map
(build-release/SakatsukuRecomp.map) and print the hottest functions.

Usage: python tools/host_sample_report.py <samples.txt> [map] [--top N]
"""
import argparse, bisect, os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def load_map(path):
    """MSVC-format map (lld-link /MAP): 'Publics by Value' and 'Static symbols'
    rows are '<sec>:<off>  <name>  <Rva+Base>  [f]  <object>'. Code only (sec 1)."""
    syms = []
    image_base = 0x140000000
    row = re.compile(r"^\s*0001:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s")
    with open(path, errors="replace") as f:
        for line in f:
            if "Preferred load address is" in line:
                image_base = int(line.split()[-1], 16)
            m = row.match(line)
            if m:
                syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    return syms, image_base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("map", nargs="?", default=os.path.join(ROOT, "build-release", "SakatsukuRecomp.map"))
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args()
    syms, image_base = load_map(a.map)
    addrs = [s[0] for s in syms]
    total = 0
    by_fn = {}
    exports = {}
    for line in open(a.samples):
        if line.startswith("#"):
            print(line.strip())
            continue
        if line.startswith("M "):
            _, mod, n = line.split(maxsplit=2)
            by_fn["[dll] " + mod] = by_fn.get("[dll] " + mod, 0) + int(n)
            total += int(n)
            continue
        if line.startswith("E "):        # within-DLL detail, already counted under [dll]
            _, fn, n = line.split(maxsplit=2)
            exports[fn] = exports.get(fn, 0) + int(n)
            continue
        rva, n = line.split()
        n = int(n)
        va = image_base + int(rva, 16)
        i = bisect.bisect_right(addrs, va) - 1
        name = syms[i][1] if i >= 0 else "?"
        by_fn[name] = by_fn.get(name, 0) + n
        total += n
    print("%d samples, %d symbols in map" % (total, len(syms)))
    for name, n in sorted(by_fn.items(), key=lambda x: -x[1])[:a.top]:
        print("%6.2f%%  %7d  %s" % (100.0 * n / total, n, name))
    if exports:
        print("-- inside ntdll/kernelbase (nearest export):")
        for name, n in sorted(exports.items(), key=lambda x: -x[1])[:15]:
            print("%6.2f%%  %7d  %s" % (100.0 * n / total, n, name))


if __name__ == "__main__":
    main()
