"""Compare match frames dumped at the same simulation ticks (SAKA_SIMTICK_SHOT)
in a 30-tick run and a 60-tick run. Reads the 1280x720 surface 0x00F00000
dumps (f<frame>_surf_<slot>_00F00000_1280x720.ppm) in each dump dir, in frame
order, and prints the mean absolute pixel difference of every 30/60 pair plus,
for scale, of consecutive frames within one run.
Usage: simcmp.py <dump30> <dump60>"""
import os, sys

def load(path):
    d = open(path, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i + 1].isspace(): i += 1
        j = i
        while not d[j:j + 1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    w, h = int(parts[1]), int(parts[2])
    return d[i + 1:i + 1 + w * h * 3]

def frames(dirpath):
    out = []
    for name in sorted(os.listdir(dirpath)):
        if name.endswith("_00F00000_1280x720.ppm"):
            out.append((name, load(os.path.join(dirpath, name))))
    return out

def mad(a, b):
    n = min(len(a), len(b))
    step = 7   # sample every 7th byte: fast and enough to rank
    s = sum(abs(a[k] - b[k]) for k in range(0, n, step))
    return s / (n // step)

A, B = frames(sys.argv[1]), frames(sys.argv[2])
print("30-tick dumps:", [n for n, _ in A])
print("60-tick dumps:", [n for n, _ in B])
for na, a in A:
    row = ["%5.1f" % mad(a, b) for _, b in B]
    print("%-40s %s" % (na[:40], " ".join(row)))
print("within 30-tick consecutive:", ["%.1f" % mad(A[k][1], A[k + 1][1]) for k in range(len(A) - 1)])
print("within 60-tick consecutive:", ["%.1f" % mad(B[k][1], B[k + 1][1]) for k in range(len(B) - 1)])
