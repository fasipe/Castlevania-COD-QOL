# The icon of Setup.exe and codport.exe, original art (nothing of the game's): a gothic castle under
# a full moon, with a bat. Drawn from shapes at 1024 pixels and averaged down into a .ico (256 to 16).
# Usage: python tools/setup/draw-icon.py tools/setup/setup   (writes tools/setup/setup.ico)
import math, struct, sys, zlib

N = 1024


def blank(c=(0, 0, 0, 0)):
    return [[c] * N for _ in range(N)]


def fill_poly(mask, pts):
    """Scanline even-odd fill of a polygon given in 0..1 coordinates into a 0/1 mask."""
    P = [(x * N, y * N) for x, y in pts]
    for row in range(N):
        y = row + 0.5
        xs = []
        for i in range(len(P)):
            (x0, y0), (x1, y1) = P[i], P[(i + 1) % len(P)]
            if (y0 <= y < y1) or (y1 <= y < y0):
                xs.append(x0 + (y - y0) * (x1 - x0) / (y1 - y0))
        xs.sort()
        for a, b in zip(xs[0::2], xs[1::2]):
            for col in range(max(0, int(a + 0.5)), min(N, int(b + 0.5))):
                mask[row][col] ^= 1


def mask_new():
    return [[0] * N for _ in range(N)]


def circle(mask, cx, cy, r):
    for row in range(N):
        y = (row + 0.5) / N - cy
        if abs(y) > r: continue
        h = math.sqrt(r * r - y * y)
        for col in range(max(0, int((cx - h) * N + 0.5)), min(N, int((cx + h) * N + 0.5))):
            mask[row][col] = 1


def rounded_rect(mask, x0, y0, x1, y1, r):
    for row in range(N):
        y = (row + 0.5) / N
        if not (y0 <= y < y1): continue
        dy = max(y0 + r - y, y - (y1 - r), 0)
        h = r - math.sqrt(max(0, r * r - dy * dy)) if dy > 0 else 0
        for col in range(max(0, int((x0 + h) * N + 0.5)), min(N, int((x1 - h) * N + 0.5))):
            mask[row][col] = 1


img = blank()


CLIP = None


def paint(mask, color):
    """color: (r,g,b,a) or a function (x,y)->(r,g,b,a)."""
    for row in range(N):
        mr = mask[row]
        if CLIP is not None:
            mr = [a & b for a, b in zip(mr, CLIP[row])]
        out = img[row]
        for col in range(N):
            if mr[col]:
                c = color((col + 0.5) / N, (row + 0.5) / N) if callable(color) else color
                a = c[3] / 255
                o = out[col]
                oa = o[3] / 255
                na = a + oa * (1 - a)
                if na <= 0: continue
                out[col] = tuple(int((c[i] * a + o[i] * oa * (1 - a)) / na + 0.5) for i in range(3)) + (int(na * 255 + 0.5),)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


# Frame: a gold edge around a dark crimson night.
m = mask_new(); rounded_rect(m, 0.02, 0.02, 0.98, 0.98, 0.17); paint(m, (184, 134, 46, 255))
m = mask_new(); rounded_rect(m, 0.045, 0.045, 0.955, 0.955, 0.15)
paint(m, lambda x, y: lerp((96, 14, 30, 255), (18, 5, 9, 255), min(1, max(0, (y - 0.05) / 0.85))))
CLIP = m
# The moon, with a faint halo.
m = mask_new(); circle(m, 0.63, 0.33, 0.25); paint(m, (242, 200, 120, 40))
m = mask_new(); circle(m, 0.63, 0.33, 0.20)
paint(m, lambda x, y: lerp((250, 228, 168, 255), (226, 178, 92, 255), min(1, math.hypot(x - 0.58, y - 0.28) / 0.25)))
# Craters.
for cx, cy, r in ((0.57, 0.29, 0.035), (0.69, 0.40, 0.025), (0.70, 0.26, 0.018)):
    m = mask_new(); circle(m, cx, cy, r); paint(m, (200, 150, 70, 110))
# A bat across the moon.
half = [(0.0, -0.030), (0.012, -0.034), (0.018, -0.055), (0.026, -0.036), (0.05, -0.042), (0.09, -0.062), (0.125, -0.058),
        (0.155, -0.030), (0.128, 0.004), (0.108, -0.012), (0.088, 0.012), (0.064, -0.004), (0.040, 0.020), (0.0, 0.030)]
bx, by = 0.63, 0.235
bat = [(bx + x, by + y) for x, y in half] + [(bx - x, by + y) for x, y in reversed(half)]
m = mask_new(); fill_poly(m, bat); paint(m, (16, 6, 10, 255))
# The castle: towers with pointed roofs, a keep with battlements, on a rock.
dark = (12, 5, 8, 255)
shapes = [
    # rock and walls
    [(0.045, 0.80), (0.12, 0.74), (0.30, 0.72), (0.50, 0.75), (0.72, 0.71), (0.90, 0.74), (0.955, 0.78), (0.955, 0.955), (0.045, 0.955)],
    # left tall tower and its spire
    [(0.15, 0.78), (0.15, 0.40), (0.135, 0.40), (0.215, 0.20), (0.295, 0.40), (0.28, 0.40), (0.28, 0.78)],
    # keep with battlements
    [(0.30, 0.78), (0.30, 0.52), (0.33, 0.52), (0.33, 0.49), (0.36, 0.49), (0.36, 0.52), (0.39, 0.52), (0.39, 0.49),
     (0.42, 0.49), (0.42, 0.52), (0.45, 0.52), (0.45, 0.49), (0.48, 0.49), (0.48, 0.52), (0.51, 0.52), (0.51, 0.49),
     (0.54, 0.49), (0.54, 0.52), (0.57, 0.52), (0.57, 0.78)],
    # central spire on the keep
    [(0.40, 0.50), (0.40, 0.42), (0.39, 0.42), (0.435, 0.30), (0.48, 0.42), (0.47, 0.42), (0.47, 0.50)],
    # right tower
    [(0.60, 0.78), (0.60, 0.47), (0.585, 0.47), (0.655, 0.29), (0.725, 0.47), (0.71, 0.47), (0.71, 0.78)],
    # small far tower
    [(0.76, 0.78), (0.76, 0.58), (0.75, 0.58), (0.80, 0.46), (0.85, 0.58), (0.84, 0.58), (0.84, 0.78)],
    # wall between the towers
    [(0.70, 0.78), (0.70, 0.64), (0.77, 0.64), (0.77, 0.78)],
]
for s in shapes:
    m = mask_new(); fill_poly(m, s); paint(m, dark)
# Lit windows.
glow = (232, 168, 64, 255)
for x0, y0, x1, y1 in ((0.20, 0.47, 0.23, 0.53), (0.20, 0.60, 0.23, 0.66), (0.42, 0.58, 0.45, 0.65), (0.64, 0.53, 0.67, 0.59),
                       (0.79, 0.63, 0.81, 0.67)):
    m = mask_new(); rounded_rect(m, x0, y0, x1, y1, 0.012); paint(m, glow)


def shrink(size):
    f = N // size
    out = []
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0, 0, 0, 0]
            for yy in range(y * f, y * f + f):
                r = img[yy]
                for xx in range(x * f, x * f + f):
                    c = r[xx]
                    a = c[3]
                    acc[0] += c[0] * a; acc[1] += c[1] * a; acc[2] += c[2] * a; acc[3] += a
            n = f * f
            a = acc[3] / n
            row.append(tuple(int(acc[i] / acc[3] + 0.5) if acc[3] else 0 for i in range(3)) + (int(a + 0.5),))
        out.append(row)
    return out


def png(pixels, size):
    raw = b''.join(b'\0' + b''.join(bytes(p) for p in row) for row in pixels)
    ch = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    return b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 9)) + ch(b'IEND', b'')


sizes = [256, 64, 48, 32, 16]
images = {s: shrink(s) for s in sizes}
# The .ico: PNG for 256, bitmaps (BGRA, bottom up, with an AND mask) for the others.
entries = []
for s in sizes:
    if s == 256:
        data = png(images[s], s)
    else:
        px = b''.join(bytes((p[2], p[1], p[0], p[3])) for row in reversed(images[s]) for p in row)
        mrow = ((s + 31) // 32) * 4
        mask = bytearray(mrow * s)
        for y, row in enumerate(reversed(images[s])):
            for x, p in enumerate(row):
                if p[3] == 0: mask[y * mrow + x // 8] |= 0x80 >> (x % 8)
        data = struct.pack('<IiiHHIIiiII', 40, s, s * 2, 1, 32, 0, 0, 0, 0, 0, 0) + px + bytes(mask)
    entries.append((s, data))
head = struct.pack('<HHH', 0, 1, len(entries))
off = 6 + 16 * len(entries)
dirs = b''
for s, data in entries:
    dirs += struct.pack('<BBBBHHII', s & 0xFF, s & 0xFF, 0, 0, 1, 32, len(data), off)
    off += len(data)
open(sys.argv[1] + '.ico', 'wb').write(head + dirs + b''.join(d for _, d in entries))
