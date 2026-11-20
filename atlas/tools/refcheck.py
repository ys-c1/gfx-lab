#!/usr/bin/env python3
"""Independent reference re-implementation of the atlas shelf packer.

Written from scratch (not derived from src/atlas.c) to cross-check the C
output block by block. Run with: python3 -I tools/refcheck.py
"""
import time

BLOCKS = [(8, 8), (12, 16), (16, 24), (20, 8),
          (24, 16), (28, 24), (32, 8), (36, 16)]
ANGLES = (0, 90, 180, 270)


def pack(rects, width, angle):
    assert angle in ANGLES, f"illegal angle {angle}"
    swap = angle in (90, 270)
    foot = [(h, w) if swap else (w, h) for (w, h) in rects]
    for idx, (rw, _) in enumerate(foot, start=1):
        if rw <= 0 or rw > width:
            return {"err_index": idx}

    shelves = []          # (x0 list..., h, used_w, tall_w, gap)
    cur, h_s, u_s, w_t = 0, 0, 0, 0
    pos = []
    for (rw, rh) in foot:
        if cur + rw > width:
            shelves.append((h_s, u_s, w_t, width - cur))
            cur, h_s, u_s, w_t = 0, 0, 0, 0
        pos.append((cur, sum(s[0] for s in shelves)))
        cur += rw
        u_s += rw
        if rh > h_s:
            h_s, w_t = rh, rw
        elif rh == h_s and rw > w_t:
            w_t = rw
    shelves.append((h_s, u_s, w_t, width - cur))  # final open shelf

    H = sum(s[0] for s in shelves)
    used = sum(w * h for (w, h) in foot)
    area = width * H
    waste = area - used
    gap = sum(g * hs for (hs, _, _, g) in shelves)
    slack = waste - gap
    bound = sum(g * hs + (u - wt) * (hs - 1)
                for (hs, u, wt, g) in shelves)
    assert gap + slack == waste
    assert used + waste == area
    assert bound >= waste, (bound, waste)
    return dict(shelves=len(shelves), H=H, used=used, area=area,
                waste=waste, rate=waste / area, gap=gap, slack=slack,
                bound=bound, bound_rate=bound / area, pos=pos,
                detail=shelves)


def main():
    for width in (128, 512):
        print(f"== page width {width} ==")
        results = {}
        for angle in ANGLES:
            t0 = time.perf_counter_ns()
            r = pack(BLOCKS, width, angle)
            dt = time.perf_counter_ns() - t0
            results[angle] = r
            print(f"  angle={angle:3d} shelves={r['shelves']} H={r['H']:3d} "
                  f"used={r['used']} area={r['area']} waste={r['waste']} "
                  f"rate={r['rate']:.4f} gap={r['gap']} slack={r['slack']} "
                  f"bound={r['bound']} bound_rate={r['bound_rate']:.4f} "
                  f"time={dt/1000:.3f}us")
            print(f"    per-shelf (h_s,u_s,w_t,g): {r['detail']}")
        best = min(ANGLES, key=lambda a: results[a]["H"])
        print(f"  best angle={best} H={results[best]['H']} "
              f"rate={results[best]['rate']:.4f}")
        print(f"  block positions: {results[best]['pos']}")
        print()

    # error-index cases mirroring test.c
    case = [(16, 16), (8, 128), (8, 8)]
    assert pack(case, 64, 90)["err_index"] == 2
    assert pack(case, 64, 270)["err_index"] == 2
    assert "err_index" not in pack(case, 64, 180)
    assert "err_index" not in pack(case, 64, 0)
    assert pack([(128, 128), (128, 128)], 64, 0)["err_index"] == 1
    try:
        pack(BLOCKS, 128, 45)
        raise AssertionError("45 deg must be rejected")
    except AssertionError as e:
        if "illegal angle" not in str(e):
            raise
    print("error-index and illegal-angle checks: OK")


if __name__ == "__main__":
    main()
