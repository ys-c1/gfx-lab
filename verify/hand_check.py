#!/usr/bin/env python3
"""Hand-calculation cross-check for the 16x16 self-crossing bowtie.

These are the SAME hand-derived expectations hard-coded as CK() assertions
in raster/test_poly.c, re-derived here independently on paper-style ray
geometry and checked against exact Fraction arithmetic. Run: python3 -I.

Polygon (vertex order, implicit close back to v0):
    v0=(2,2)  v1=(14,14)  v2=(14,2)  v3=(2,14)
Pixel centres are (x+0.5, y+0.5). The ray runs to +x with half-open edges:
upward edge a.y<=py<b.y counts when the point is strictly left of it,
downward edge b.y<=py<a.y when strictly right (Sunday winding rule).
"""
from fractions import Fraction as F

V = [(F(2), F(2)), (F(14), F(14)), (F(14), F(2)), (F(2), F(14))]


def state(px, py):
    wn = cr = 0
    for k in range(4):
        ax, ay = V[k]
        bx, by = V[(k + 1) % 4]
        is_left = (bx - ax) * (py - ay) - (by - ay) * (px - ax)
        if ay <= py:
            if by > py and is_left > 0:
                wn += 1
                cr += 1
        else:
            if by <= py and is_left < 0:
                wn -= 1
                cr += 1
    return cr, wn


# (x, y) -> (expected crossing count, expected winding, reason)
HAND = {
    (12, 3): (1, -1, "right CW lobe: one downward crossing, winding -1"),
    (4, 10): (3,  1, "left CCW lobe: three crossings, two up one down -> +1"),
    (3, 12): (2,  0, "ray clips lobe edge twice: even count, winding cancels"),
    (8, 8):  (1, -1, "right lobe interior near the crossing diagonal"),
    (13, 3): (1, -1, "right lobe, single downward crossing"),
    (0, 0):  (0,  0, "outside the bounding box entirely"),
}


def main():
    fails = 0
    print("hand calc table for bowtie on 16x16 grid")
    print(f"{'pixel':8} {'centre':14} {'hand(cr,wn)':14} {'exact(cr,wn)':14} ok")
    for (x, y), (ecr, ewn, why) in HAND.items():
        px, py = F(x) + F(1, 2), F(y) + F(1, 2)
        cr, wn = state(px, py)
        ok = (cr, wn) == (ecr, ewn)
        fails += not ok
        print(f"({x:2d},{y:2d})  ({float(px):4.1f},{float(py):4.1f})  "
              f"({ecr:2d},{ewn:2d})       ({cr:2d},{wn:2d})       "
              f"{'yes' if ok else 'NO '}  {why}")
    print(f"\n{len(HAND)} hand samples, {fails} failures")
    raise SystemExit(1 if fails else 0)


if __name__ == "__main__":
    main()
