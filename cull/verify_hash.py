#!/usr/bin/env python3
"""Independent reference for the spatial-hash cull (run with python3 -I).

Rebuilds the bucket table from the exported scene using only the standard
library, then checks the C side against it:

  * brute-force visible set (per-object overlap, the original cull_run rule);
  * the grid, occupied/entries/collisions/collision-rate numbers printed in
    the exported bucket table, recomputed line by line;
  * every bucket's (bucket_x, bucket_y), count, and ascending object ids,
    compared against the table the C library wrote;
  * candidate count for the query (the reference dedups bucket membership);
  * exact equality of the C hash result and the C brute result with the
    Python brute result, element by element.

The scene, both result lines and the bucket table are all written by
cull/bench.c.  Any mismatch is a failure.
"""
import os
import sys


def overlaps(box, view):
    x, y, w, h = box
    vx, vy, vw, vh = view
    return not (x + w < vx or x > vx + vw or y + h < vy or y > vy + vh)


def read_scene(path):
    view, boxes = None, []
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "view":
                view = tuple(float(v) for v in p[1:5])
            elif p[0] == "n":
                pass
            elif p[0] == "box":
                idx = int(p[1])
                x, y, w, h = (float(v) for v in p[2:6])
                boxes.append((idx, (x, y, w, h)))
    boxes.sort()
    assert [i for i, _ in boxes] == list(range(len(boxes))), "box indices not contiguous"
    return view, [b for _, b in boxes]


def read_buckets(path):
    meta, buckets = {}, []
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p or p[0].startswith("#"):
                continue
            if p[0] == "bucket":
                bx, by, cnt = int(p[1]), int(p[2]), int(p[3])
                ids = [int(v) for v in p[4:]]
                assert len(ids) == cnt, f"{path}: bucket ({bx},{by}) count mismatch"
                buckets.append((bx, by, ids))
            else:
                meta[p[0]] = p[1:]
    meta["buckets"] = list(map(int, meta["buckets"]))
    meta["occupied"] = int(meta["occupied"][0])
    meta["entries"] = int(meta["entries"][0])
    meta["collisions"] = int(meta["collisions"][0])
    meta["collision_rate"] = float(meta["collision_rate"][0])
    meta["grid"] = tuple(map(int, meta["grid"]))
    meta["origin"] = tuple(map(float, meta["origin"]))
    meta["cell"] = tuple(map(float, meta["cell"]))
    meta["n"] = int(meta["n"][0])
    return meta, buckets


def bucket_range(lo, hi, origin, cell, dim):
    """Inclusive cell range for the closed interval [lo, hi]."""
    f0 = (lo - origin) / cell
    f1 = (hi - origin) / cell
    # floor without importing math.floor's float surprises: these are finite.
    i0 = int(f0 // 1)
    i1 = int(f1 // 1)
    if i1 < 0 or i0 > dim - 1:
        return None
    i0 = max(i0, 0)
    i1 = min(i1, dim - 1)
    return i0, i1


def rebuild(boxes, meta):
    gw, gh = meta["grid"]
    ox, oy = meta["origin"]
    cw, ch = meta["cell"]
    cells = {}
    for i, (x, y, w, h) in enumerate(boxes):
        rx = bucket_range(x, x + w, ox, cw, gw)
        ry = bucket_range(y, y + h, oy, ch, gh)
        if rx is None or ry is None:
            continue
        for gy in range(ry[0], ry[1] + 1):
            for gx in range(rx[0], rx[1] + 1):
                cells.setdefault((gx, gy), []).append(i)
    entries = sum(len(v) for v in cells.values())
    occupied = len(cells)
    collisions = entries - occupied
    rate = 100.0 * collisions / entries if entries else 0.0
    return cells, entries, occupied, collisions, rate


def read_results(path):
    out = {}
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] in ("brute", "hash"):
            cnt = int(p[1])
            ids = [int(v) for v in p[2:]]
            assert len(ids) == cnt, f"{path}: {p[0]} count mismatch"
            out[p[0]] = ids
        elif p[0] == "stats":
            out["stats"] = dict(kv.split("=") for kv in p[1:])
    return out


def check_scale(art, n):
    scene = os.path.join(art, f"cull_hash_scene_{n}.txt")
    result = os.path.join(art, f"cull_hash_result_{n}.txt")
    table = os.path.join(art, f"cull_hash_buckets_{n}.txt")
    view, boxes = read_scene(scene)
    assert len(boxes) == n, f"scene has {len(boxes)} boxes, expected {n}"

    ref_visible = [i for i, b in enumerate(boxes) if overlaps(b, view)]
    res = read_results(result)
    c_brute, c_hash = res["brute"], res["hash"]

    ok = True
    if c_brute != ref_visible:
        ok = False
        print(f"  !! n={n}: C brute differs from Python reference "
              f"(C {len(c_brute)}, ref {len(ref_visible)})")
    if c_hash != ref_visible:
        ok = False
        print(f"  !! n={n}: C hash differs from Python reference")
    if c_hash != c_brute:
        ok = False
        print(f"  !! n={n}: C hash differs from C brute")

    meta, buckets = read_buckets(table)
    cells, entries, occupied, collisions, rate = rebuild(boxes, meta)
    if (meta["buckets"][0] != meta["grid"][0] * meta["grid"][1]):
        ok = False
        print(f"  !! n={n}: bucket count {meta['buckets'][0]} != grid product")
    if entries != meta["entries"]:
        ok = False
        print(f"  !! n={n}: entries C {meta['entries']} ref {entries}")
    if occupied != meta["occupied"]:
        ok = False
        print(f"  !! n={n}: occupied C {meta['occupied']} ref {occupied}")
    if collisions != meta["collisions"]:
        ok = False
        print(f"  !! n={n}: collisions C {meta['collisions']} ref {collisions}")
    if abs(rate - meta["collision_rate"]) > 0.005 + 1e-12:
        ok = False
        print(f"  !! n={n}: collision rate C {meta['collision_rate']} ref {rate:.2f}")

    # Compare bucket by bucket, including in-bucket order.
    ref_list = sorted(((bx, by, ids) for (bx, by), ids in cells.items()),
                     key=lambda t: (t[1], t[0]))
    if len(ref_list) != len(buckets):
        ok = False
        print(f"  !! n={n}: exported {len(buckets)} buckets, reference {len(ref_list)}")
    else:
        for (bx, by, ids), (rx, ry, rids) in zip(buckets, ref_list):
            if (bx, by) != (rx, ry):
                ok = False
                print(f"  !! n={n}: bucket order mismatch at ({bx},{by}) vs ({rx},{ry})")
                break
            if ids != rids:
                ok = False
                print(f"  !! n={n}: bucket ({bx},{by}) ids differ: C {ids} ref {rids}")
                break
            if ids != sorted(ids):
                ok = False
                print(f"  !! n={n}: bucket ({bx},{by}) not ascending")
                break

    # Candidates implied by the query: dedup objects in buckets the view covers.
    gw, gh = meta["grid"]
    rx = bucket_range(view[0], view[0] + view[2], meta["origin"][0], meta["cell"][0], gw)
    ry = bucket_range(view[1], view[1] + view[3], meta["origin"][1], meta["cell"][1], gh)
    cand = set()
    visited = 0
    if rx is not None and ry is not None:
        for gy in range(ry[0], ry[1] + 1):
            for gx in range(rx[0], rx[1] + 1):
                visited += 1
                cand.update(cells.get((gx, gy), []))
    if int(res["stats"]["candidates"]) != len(cand):
        ok = False
        print(f"  !! n={n}: candidates C {res['stats']['candidates']} ref {len(cand)}")
    if int(res["stats"]["visited_buckets"]) != visited:
        ok = False
        print(f"  !! n={n}: visited buckets C {res['stats']['visited_buckets']} ref {visited}")
    if int(res["stats"]["collisions"]) != collisions:
        ok = False
        print(f"  !! n={n}: result-line collisions disagree")
    if abs(float(res["stats"]["collision_rate"]) - round(rate, 2)) > 1e-9:
        ok = False
        print(f"  !! n={n}: result-line collision rate disagrees")

    print(f"  n={n}: 可见 {len(ref_visible)}，候选 {len(cand)}，"
          f"桶 {meta['buckets']}（非空 {occupied}），插入 {entries}，"
          f"冲突 {collisions}，冲突率 {rate:.2f}%  "
          f"{'PASS' if ok else 'FAIL'}")
    return ok


def main():
    art = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "artifacts")
    art = os.path.abspath(art)
    print("================ 独立参考复核（纯标准库） ================")
    ok = True
    for n in (1000, 10000):
        ok &= check_scale(art, n)
    print("总判定:", "全部 PASS" if ok else "存在 FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
