"""Compare two PPU_STEP_CENSUS logs (30 ticks vs 60 ticks).

For every function: the example int/float step it reported in each run (most
common value), and the ratio 60/30. A float step that halves at 60 ticks is
time-based already; one that stays the same is a fixed per-tick step that
needs halving under SAKA_LOGIC60. Usage: cencmp.py <log30> <log60>
"""
import re, sys, collections

ROW = re.compile(r"\[step-census\] fn=0x([0-9A-F]+) steps/frame\s+([\d.]+) \(int\s+([\d.]+), float\s+([\d.]+)\) "
                 r"stores/frame\s+([\d.]+)\s+int ex \[0x([0-9A-F]+)\] ([+-]?\d+)\s+float ex \[0x([0-9A-F]+)\] (\S+)")

def load(path):
    out = collections.defaultdict(lambda: {"n": 0, "steps": 0.0, "ints": collections.Counter(), "floats": collections.Counter(),
                                           "iaddr": collections.Counter(), "faddr": collections.Counter()})
    for line in open(path, errors="replace"):
        m = ROW.search(line)
        if not m:
            continue
        fn = m.group(1)
        e = out[fn]
        e["n"] += 1
        e["steps"] = max(e["steps"], float(m.group(2)))
        iv = int(m.group(7))
        fv = float(m.group(9))
        if float(m.group(3)) > 0 and iv:
            e["ints"][iv] += 1
            e["iaddr"][m.group(6)] += 1
        if float(m.group(4)) > 0 and fv and abs(fv) > 1e-30:
            e["floats"][round(fv, 6)] += 1
            e["faddr"][m.group(8)] += 1
    return out

def top(c):
    return c.most_common(1)[0][0] if c else None

a, b = load(sys.argv[1]), load(sys.argv[2])
fns = sorted(set(a) | set(b), key=lambda f: -(max(a[f]["steps"] if f in a else 0, b[f]["steps"] if f in b else 0)))
print("%-10s %7s %7s | %12s %12s %6s | %6s %6s | %s" % ("fn", "st30", "st60", "float30", "float60", "ratio", "int30", "int60", "verdict"))
for f in fns:
    ea, eb = a.get(f), b.get(f)
    f30 = top(ea["floats"]) if ea else None
    f60 = top(eb["floats"]) if eb else None
    i30 = top(ea["ints"]) if ea else None
    i60 = top(eb["ints"]) if eb else None
    ratio = (f60 / f30) if (f30 and f60) else None
    if ratio is not None and abs(ratio - 0.5) < 0.05:
        verdict = "time-based (halved)"
    elif ratio is not None and abs(ratio - 1.0) < 0.05:
        verdict = "FIXED per tick"
    elif ratio is not None:
        verdict = "other %.2f" % ratio
    elif i30 is not None and i60 is not None:
        verdict = "int same" if i30 == i60 else "int %d->%d" % (i30, i60)
    elif ea and not eb:
        verdict = "only 30"
    elif eb and not ea:
        verdict = "only 60"
    else:
        verdict = "?"
    print("%-10s %7.1f %7.1f | %12s %12s %6s | %6s %6s | %s" % (
        f, ea["steps"] if ea else 0, eb["steps"] if eb else 0,
        "%.6g" % f30 if f30 is not None else "-", "%.6g" % f60 if f60 is not None else "-",
        "%.2f" % ratio if ratio is not None else "-",
        i30 if i30 is not None else "-", i60 if i60 is not None else "-", verdict))
