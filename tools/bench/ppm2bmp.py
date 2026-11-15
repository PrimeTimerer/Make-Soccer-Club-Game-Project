import sys, os, struct
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
for name in sorted(os.listdir(src)):
    if not name.endswith('.ppm'): continue
    d = open(os.path.join(src, name), 'rb').read()
    # P6\n<w> <h>\n<max>\n
    parts = []; i = 0
    while len(parts) < 4:
        while d[i:i+1].isspace(): i += 1
        if d[i:i+1] == b'#':
            while d[i:i+1] != b'\n': i += 1
            continue
        j = i
        while not d[j:j+1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    px = d[i:i + w * h * 3]
    row = (w * 3 + 3) & ~3
    out = bytearray(b'BM' + struct.pack('<IHHI', 54 + row * h, 0, 0, 54) + struct.pack('<IiiHHIIiiII', 40, w, -h, 1, 24, 0, row * h, 0, 0, 0, 0))
    for y in range(h):
        r = px[y * w * 3:(y + 1) * w * 3]
        b = bytearray(r)
        b[0::3], b[2::3] = r[2::3], r[0::3]
        out += b + b'\0' * (row - w * 3)
    open(os.path.join(dst, name[:-4] + '.bmp'), 'wb').write(out)
print('ok')
