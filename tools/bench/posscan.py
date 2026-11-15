"""Analyse the per-frame logs/posscan.txt: floats that change only with the
30 Hz match tick. Prints where they live (64 KB blocks), the moving 2D/3D
coordinate pairs among them, and the address strides those pairs repeat at."""
import math, sys
from collections import Counter, defaultdict

ticks = None
vals = {}
for line in open(sys.argv[1]):
    if line.startswith("#"):
        ticks = [int(t) for t in line.split()[2:]]
        continue
    p = line.split()
    vals[int(p[0], 16)] = [float(v) for v in p[1:]]
print("ticks per frame:", ticks)
print("match-tick-only floats:", len(vals))
blocks = Counter(a >> 16 for a in vals)
print("by 64 KB block:", ", ".join("%04X0000:%d" % kv for kv in blocks.most_common(16)))

def steps(v):
    return [v[i + 1] - v[i] for i in range(len(v) - 1) if ticks[i + 1] != ticks[i]]

S = set(vals)
pairs = []
for a in sorted(vals):
    for gap in (4, 8):
        b = a + gap
        if b not in S:
            continue
        if gap == 8 and a + 4 in S:
            continue
        x, z = vals[a], vals[b]
        if abs(x[-1]) > 60 or abs(z[-1]) > 60:
            continue
        sx, sz = steps(x), steps(z)
        mv = [math.hypot(sx[i], sz[i]) for i in range(len(sx))]
        if not mv or max(mv) > 2.0 or max(mv) < 0.005:
            continue
        pairs.append((a, gap, x[-1], z[-1], sum(mv) / len(mv)))
print("moving coordinate pairs:", len(pairs))
for gap in (4, 8):
    eas = sorted(p[0] for p in pairs if p[1] == gap)
    hist = Counter()
    for i, a in enumerate(eas):
        for b in eas[i + 1:i + 30]:
            if b - a >= 0x20:
                hist[b - a] += 1
    print("gap %d: %d pairs, top strides %s" % (gap, len(eas), ", ".join("0x%X x%d" % kv for kv in hist.most_common(6))))
for a, gap, x, z, m in pairs[:120]:
    print("  %08X gap %d  (%7.2f, %7.2f)  step %.3f" % (a, gap, x, z, m))
