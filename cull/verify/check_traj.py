#!/usr/bin/env python3
"""Independent verifier for the cull-module trajectory exports.

Recomputes everything from the exported box tables and keyframe views
(parsed out of the exported trajectories), then cross-checks:

  1. incremental trajectory vs full-rebuild trajectory (must be identical)
  2. every C-computed frame against this script's own overlap test
  3. the small scene against the hand-computed table handcheck.csv
  4. candidate counts, rebuilt flags, added/removed/unchanged accounting
  5. interpolated view coefficients t and view coordinates

Usage: python3 check_traj.py <bench_dir> <handcheck.csv>
Exit code 0 only if every check passes.
"""
import csv
import math
import os
import sys


def read_boxes(path):
    boxes = []
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            boxes.append((float(row["x"]), float(row["y"]),
                          float(row["w"]), float(row["h"]), int(row["layer"])))
    return boxes


def read_traj(path):
    rows = []
    with open(path, newline="") as fh:
        reader = csv.DictReader(
            line for line in fh if line.strip() and not line.lstrip().startswith("#"))
        for row in reader:
            ids = tuple(int(x) for x in row["visible_ids"].split("|")) if row["visible_ids"] else ()
            rows.append({
                "frame": int(row["frame"]),
                "t": float(row["t"]),
                "vx": float(row["vx"]), "vy": float(row["vy"]),
                "vw": float(row["vw"]), "vh": float(row["vh"]),
                "candidates": int(row["candidates"]),
                "rebuilt": int(row["rebuilt"]),
                "added": int(row["added"]), "removed": int(row["removed"]),
                "unchanged": int(row["unchanged"]),
                "visible_count": int(row["visible_count"]),
                "ids": ids,
            })
    return rows


def overlaps(b, vx, vy, vw, vh):
    bx, by, bw, bh, _ = b
    return not (bx + bw < vx or bx > vx + vw or by + bh < vy or by > vy + vh)


def lerp(a, b, t):
    return a + (b - a) * t


def main():
    bench_dir, hand_path = sys.argv[1], sys.argv[2]
    failures = []
    checks = 0

    def expect(cond, msg):
        nonlocal checks
        checks += 1
        if not cond:
            failures.append(msg)

    for scene in ("small", "large"):
        boxes = read_boxes(os.path.join(bench_dir, f"{scene}_boxes.csv"))
        inc = read_traj(os.path.join(bench_dir, f"{scene}_traj_inc.csv"))
        full = read_traj(os.path.join(bench_dir, f"{scene}_traj_full.csv"))
        n = len(boxes)

        v0x, v0y = inc[0]["vx"], inc[0]["vy"]
        v0w, v0h = inc[0]["vw"], inc[0]["vh"]
        v1x, v1y = inc[-1]["vx"], inc[-1]["vy"]
        v1w, v1h = inc[-1]["vw"], inc[-1]["vh"]

        # csv exports both t and coords at 6 decimals. The C side computes
        # coords from the full-precision t before rounding each independently,
        # so recomputing here from the rounded t can differ by up to
        # span * 5e-7 plus two 5e-7 coordinate roundings (~1.3e-5 at span 100).
        span = max(abs(v1x - v0x), abs(v1y - v0y),
                   abs(v1w - v0w), abs(v1h - v0h), 1.0)
        tol = span * 5e-7 + 1e-5

        # independently derived candidate set: boxes overlapping union(v0,v1)
        ux, uy = min(v0x, v1x), min(v0y, v1y)
        uw = max(v0x + v0w, v1x + v1w) - ux
        uh = max(v0y + v0h, v1y + v1h) - uy
        want_cand = [i for i, b in enumerate(boxes) if overlaps(b, ux, uy, uw, uh)]

        expect(len(inc) == len(full), f"{scene}: frame count differs")
        nf = min(len(inc), len(full))
        id_errors = 0
        total_ids = 0
        prev = ()
        for k in range(nf):
            pi, pf = inc[k], full[k]
            t = pi["t"]

            # 1. both modes must agree exactly
            expect(pi["ids"] == pf["ids"],
                   f"{scene} f{k}: inc/full ids differ {pi['ids']} vs {pf['ids']}")
            expect(pf["rebuilt"] == 1, f"{scene} f{k}: full mode rebuilt flag not 1")

            # 2. independent recomputation from boxes + interpolated view
            vx = lerp(v0x, v1x, t); vy = lerp(v0y, v1y, t)
            vw = lerp(v0w, v1w, t); vh = lerp(v0h, v1h, t)
            want = tuple(i for i, b in enumerate(boxes) if overlaps(b, vx, vy, vw, vh))
            total_ids += len(want)
            if want != pi["ids"]:
                id_errors += 1

            # 3. candidate set and rebuild accounting.
            # view coords are exported at 6 decimals independently from t;
            # tolerance covers both roundings (see span/tol above)
            expect(math.isclose(vx, pi["vx"], abs_tol=tol) and
                   math.isclose(vy, pi["vy"], abs_tol=tol),
                   f"{scene} f{k}: interpolated view position mismatch")
            expect(math.isclose(vw, pi["vw"], abs_tol=tol) and
                   math.isclose(vh, pi["vh"], abs_tol=tol),
                   f"{scene} f{k}: interpolated view size mismatch")
            expect(pi["candidates"] == len(want_cand),
                   f"{scene} f{k}: candidate count {pi['candidates']} != {len(want_cand)}")
            if scene == "small":
                expect(pi["rebuilt"] == 0,
                       f"{scene} f{k}: incremental mode should never fully rebuild")

            # 4. added/removed/unchanged via sorted-set diff against previous
            add = len(set(want) - set(prev))
            rem = len(set(prev) - set(want))
            unc = len(set(want) & set(prev))
            expect(pi["added"] == add and pi["removed"] == rem and pi["unchanged"] == unc,
                   f"{scene} f{k}: diff accounting "
                   f"got({pi['added']},{pi['removed']},{pi['unchanged']}) "
                   f"want({add},{rem},{unc})")
            expect(pi["visible_count"] == len(pi["ids"]),
                   f"{scene} f{k}: visible_count disagrees with id list")
            expect(list(pi["ids"]) == sorted(pi["ids"]),
                   f"{scene} f{k}: ids not sorted")
            prev = want

        rate = id_errors / total_ids if total_ids else 0.0
        expect(id_errors == 0,
               f"{scene}: {id_errors} frames with id-set errors vs python recompute")
        print(f"[{scene}] n={n} frames={nf} candidates={len(want_cand)} "
              f"({100.0*len(want_cand)/n:.2f}% of n) "
              f"rebuilds inc={sum(r['rebuilt'] for r in inc)} "
              f"full={sum(r['rebuilt'] for r in full)} "
              f"id-error-rate={rate:.3e}")

    # 5. hand-computed table vs the dedicated 5-keyframe export
    # (small_key_traj_inc.csv): identical sampling, exact row-by-row equality
    hand = read_traj(hand_path)
    key = read_traj(os.path.join(bench_dir, "small_key_traj_inc.csv"))
    expect(len(hand) == len(key),
           f"handcheck: row count {len(hand)} != export {len(key)}")
    for h, c in zip(hand, key):
        for fld in ("frame", "t", "candidates", "rebuilt", "added", "removed",
                    "unchanged", "visible_count", "ids"):
            expect(h[fld] == c[fld],
                   f"handcheck f{h['frame']}: {fld} {h[fld]} != {c[fld]}")
        for fld in ("vx", "vy", "vw", "vh"):
            expect(math.isclose(h[fld], c[fld], abs_tol=5e-7),
                   f"handcheck f{h['frame']}: {fld} {h[fld]} != {c[fld]}")
    print(f"[handcheck] {len(hand)} rows compared one-to-one with "
          f"small_key_traj_inc.csv (all fields incl. added/removed)")

    print(f"\n{checks} checks, {len(failures)} failures")
    for msg in failures:
        print("  FAIL:", msg)
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(main())
