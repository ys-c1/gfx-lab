#!/usr/bin/env python3
"""Independent hand calculation of near-plane clipping, cross-checked against
the C module's formulas. Run with -I; reads nothing untrusted."""
import math

def hit(a, b, near):
    t = (near - a[2]) / (b[2] - a[2])
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, near)

def clip(tri, near):
    poly = []
    for e in range(3):
        a, b = tri[e], tri[(e + 1) % 3]
        ain, bin_ = a[2] >= near, b[2] >= near
        if ain and bin_:
            poly.append(b)
        elif ain and not bin_:
            poly.append(hit(a, b, near))
        elif not ain and bin_:
            poly.append(hit(a, b, near)); poly.append(b)
    return poly

def edge(a, b, p):
    return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])

def fill_count(tri, w, h):
    """Pixel centers (x+.5,y+.5); same top-left fill rule as raster.c."""
    if abs(edge(tri[0], tri[1], tri[2])) < 1e-12:
        return 0
    n = 0
    for y in range(h):
        for x in range(w):
            px, py = x + 0.5, y + 0.5
            w0 = edge(tri[1], tri[2], (px, py))
            w1 = edge(tri[2], tri[0], (px, py))
            w2 = edge(tri[0], tri[1], (px, py))
            if w0 < 0 or w1 < 0 or w2 < 0:
                continue
            n += 1
    return n

def approx(p, q, eps=1e-9):
    return all(abs(a - b) < eps for a, b in zip(p, q))

fails = 0
def check(name, got, want):
    global fails
    ok = got == want
    if not ok:
        fails += 1
    print(f"{'ok ' if ok else 'FAIL'} {name}: got={got} want={want}")

# Case A: 1 inside -> triangle, 2 new verts
a = [(0,0,2), (4,0,0), (0,4,0)]
pa = clip(a, 1.0)
check("A polygon size", len(pa), 3)
check("A hit on v0-v1", any(approx(p, (2,0,1)) for p in pa), True)
check("A hit on v2-v0", any(approx(p, (0,2,1)) for p in pa), True)
check("A kept v0", any(approx(p, (0,0,2)) for p in pa), True)

# Case B: 2 inside -> quad, 2 new verts, fan = 2 triangles
b = [(0,0,2), (8,0,2), (4,4,0)]
pb = clip(b, 1.0)
check("B polygon size", len(pb), 4)
check("B hit 1", any(approx(p, (6,2,1)) for p in pb), True)
check("B hit 2", any(approx(p, (2,2,1)) for p in pb), True)
check("B fan triangles", len(pb) - 2, 2)

# Case: all behind -> empty
check("all behind empty", clip([(0,0,.1),(4,0,.2),(0,4,.3)], 1.0), [])

# New-vertex count = polygon points not coincident with an input vertex
def new_verts(tri, near):
    poly = clip(tri, near)
    return sum(1 for p in poly if not any(approx(p, v) for v in tri))

check("A new verts", new_verts(a, 1.0), 2)
check("B new verts", new_verts(b, 1.0), 2)
onplane = [(0,0,1), (4,0,1), (0,4,3)]
check("on-plane new verts", new_verts(onplane, 1.0), 0)

# Pixel fill hand count: triangle (0,0),(8,0),(0,8) -> x+y<=7 -> 8+7+..+1 = 36
check("fill 36", fill_count([(0,0,.5),(8,0,.5),(0,8,.5)], 16, 16), 36)

# Clipped triangle A rasterized: its surviving tri is (0,0,2),(2,0,1),(0,2,1)
# on a 16x16 target -> x+y <= 1 -> 2+1 = 3 pixels
check("clipped A fill", fill_count([(0,0,2),(2,0,1),(0,2,1)], 16, 16), 3)

# Resolution scaling sanity: same shape at 8x linear scale -> ~64x pixels
small = fill_count([(0,0,.5),(8,0,.5),(0,8,.5)], 16, 16)
big_tri = [(0,0,.5),(64,0,.5),(0,64,.5)]
big = fill_count(big_tri, 128, 128)
print(f"ok  scaling: 16px-target fill={small}, 8x geometry fill={big}, ratio={big/small:.2f} (area ratio 64)")

print("ALL HAND CHECKS PASSED" if fails == 0 else f"{fails} FAILURES")
raise SystemExit(1 if fails else 0)
