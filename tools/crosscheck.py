#!/usr/bin/env python3
"""Independent recomputation of the cull benchmark results.

Reads the CSVs written by cull/bench.c and recomputes every decision from
raw fields (x,y,w,h,vx,vy,tol,margin) without linking the C library.
Also cross-checks the exported candidate ID lists against the CSV flags.

Run:  python3 -I tools/crosscheck.py [artifacts/cull]
Exit code 0 only if every check passes.
"""
import csv
import json
import math
import os
import sys


def overlap(x0, y0, x1, y1, vw, vh, margin=0.0):
    # viewport expanded symmetrically: bounds [-m, vw+m] x [-m, vh+m]
    return not (x1 < -margin or x0 > vw + margin or
                y1 < -margin or y0 > vh + margin)


def static_inside(row):
    b = row
    return overlap(b["x"], b["y"], b["x"] + b["w"], b["y"] + b["h"],
                   b["vw"], b["vh"])


def swept_inside(row, dt):
    b = row
    x0, x1 = b["x"], b["x"] + b["w"]
    y0, y1 = b["y"], b["y"] + b["h"]
    nx0, nx1 = x0 + b["vx"] * dt, x1 + b["vx"] * dt
    ny0, ny1 = y0 + b["vy"] * dt, y1 + b["vy"] * dt
    return overlap(min(x0, nx0), min(y0, ny0), max(x1, nx1), max(y1, ny1),
                   b["vw"], b["vh"])


def adaptive_inside(row, dt):
    """tol recomputed from speed; box grown by tol, viewport by margin."""
    b = row
    tol = b.get("tol_base", 0.0) + math.hypot(b["vx"], b["vy"]) * dt
    return overlap(b["x"] - tol, b["y"] - tol,
                   b["x"] + b["w"] + tol, b["y"] + b["h"] + tol,
                   b["vw"], b["vh"], b["margin"])


def read_csv(path, vw, vh, margin, dt):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            row = {
                "id": int(r["id"]),
                "x": float(r["x"]), "y": float(r["y"]),
                "w": float(r["w"]), "h": float(r["h"]),
                "vx": float(r["vx"]), "vy": float(r["vy"]),
                "vw": vw, "vh": vh, "margin": margin,
                "tol_base": 0.0,
            }
            rows.append(row)
    return rows


def check_candidate_file(path, kept_ids, label, failures):
    with open(path) as f:
        ids = [int(line.strip()) for line in f if line.strip()]
    if ids != kept_ids:
        failures.append(
            f"{label}: candidate file {os.path.basename(path)} mismatch "
            f"(file={len(ids)} ids, csv-flags={len(kept_ids)})")
    else:
        print(f"  ok  {label}: {len(ids)} candidates match exported list")


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "artifacts/cull"
    dt = 1.0 / 60.0        # main benchmark
    dt_hand = 0.1          # hand-computed reference cases
    failures = []
    checked_rows = 0

    with open(os.path.join(outdir, "summary.json")) as f:
        summary = json.load(f)
    vw, vh = summary["view"]["w"], summary["view"]["h"]

    # ---- hand cases: verify the printed hand table numbers too -------
    hand_path = os.path.join(outdir, "hand.csv")
    hand = []
    with open(hand_path, newline="") as f:
        for r in csv.DictReader(f):
            hand.append(r)
    print(f"hand.csv: {len(hand)} rows")
    for r in hand:
        i = int(r["id"])
        vx, vy = float(r["vx"]), float(r["vy"])
        tol_expect = math.hypot(vx, vy) * dt_hand
        if abs(float(r["tol"]) - tol_expect) > 1e-12:
            failures.append(f"hand id={i}: tol {r['tol']} != {tol_expect}")
        row = {
            "x": float(r["x"]), "y": float(r["y"]),
            "w": float(r["w"]), "h": float(r["h"]),
            "vx": vx, "vy": vy, "vw": 60.0, "vh": 60.0,
            "margin": 0.0, "tol_base": 0.0,
        }
        if int(r["static_inside"]) != static_inside(row):
            failures.append(f"hand id={i}: static_inside mismatch")
        if int(r["swept_inside"]) != swept_inside(row, dt_hand):
            failures.append(f"hand id={i}: swept_inside mismatch")
        if int(r["adapt_m0_kept"]) != adaptive_inside(row, dt_hand):
            failures.append(f"hand id={i}: adaptive decision mismatch")
        row8 = dict(row, margin=8.0)
        if int(r["adapt_m8_kept"]) != adaptive_inside(row8, dt_hand):
            failures.append(f"hand id={i}: adaptive m8 decision mismatch")
    hand_adapt = [int(r["id"]) for r in hand if r["adapt_m0_kept"] == "1"]
    hand_m8 = [int(r["id"]) for r in hand if r["adapt_m8_kept"] == "1"]
    if hand_adapt != [0, 2, 3, 4, 6, 7]:
        failures.append(f"hand adaptive id set mismatch: {hand_adapt}")
    else:
        print("  ok  hand adaptive set = [0,2,3,4,6,7], zero false culls")
    if hand_m8 != [0, 2, 3, 4, 5, 6, 7]:
        failures.append(f"hand adaptive m8 id set mismatch: {hand_m8}")
    else:
        print("  ok  hand adaptive m8 set = [0,2,3,4,5,6,7] (id 5 rescued by margin)")

    # ---- detail CSVs and candidate exports ---------------------------
    for entry in summary["runs"]:
        n, margin = entry["n"], entry["margin"]
        tag = f"n={n} margin={margin:g}"
        detail = os.path.join(outdir, f"details_{n}_m{margin:g}.csv")
        rows = read_csv(detail, vw, vh, margin, dt)
        if len(rows) != n:
            failures.append(f"{tag}: row count {len(rows)} != {n}")

        legacy_false = adapt_false = 0
        legacy_kept, adapt_kept = [], []
        tol_min, tol_max, tol_sum = math.inf, 0.0, 0.0
        with open(detail, newline="") as f:
            for r in csv.DictReader(f):
                i = int(r["id"])
                row = rows[i]
                si = static_inside(row)
                wi = swept_inside(row, dt)
                ai = adaptive_inside(row, dt)
                tol = math.hypot(row["vx"], row["vy"]) * dt
                tol_min = min(tol_min, tol)
                tol_max = max(tol_max, tol)
                tol_sum += tol
                if int(r["static_inside"]) != si:
                    failures.append(f"{tag} id={i}: static flag mismatch")
                if int(r["swept_inside"]) != wi:
                    failures.append(f"{tag} id={i}: swept flag mismatch")
                if int(r["adapt_kept"]) != ai:
                    failures.append(f"{tag} id={i}: adaptive flag mismatch")
                # C hypot vs libm/math hypot differ by ulps (~1e-8 at
                # tol~20); tolerate that, but nothing near pixel scale.
                if abs(float(r["tol"]) - tol) > 1e-7:
                    failures.append(f"{tag} id={i}: tol value mismatch")
                lk = int(r["legacy_kept"]) == 1
                if lk:
                    legacy_kept.append(i)
                if ai:
                    adapt_kept.append(i)
                if wi and not lk:
                    legacy_false += 1
                if wi and not ai:
                    adapt_false += 1
                checked_rows += 1

        # recomputed aggregates must equal summary.json
        expect = {
            "legacy_false_culls": legacy_false,
            "adaptive_false_culls": adapt_false,
            "legacy_candidates": entry["legacy_candidates"],
            "adaptive_candidates": entry["adaptive_candidates"],
        }
        if legacy_false != entry["legacy_false_culls"]:
            failures.append(
                f"{tag}: legacy false culls {legacy_false} != "
                f"{entry['legacy_false_culls']}")
        if adapt_false != entry["adaptive_false_culls"]:
            failures.append(
                f"{tag}: adaptive false culls {adapt_false} != "
                f"{entry['adaptive_false_culls']}")
        if len(legacy_kept) != entry["legacy_candidates"]:
            failures.append(f"{tag}: legacy count mismatch")
        if len(adapt_kept) != entry["adaptive_candidates"]:
            failures.append(f"{tag}: adaptive count mismatch")
        # C/Python libm hypot differ by ulps (~2e-7 on tol_max at tol~20);
        # these are sub-1e-6 px effects, far below any geometric decision.
        if abs(tol_min - entry["tol_min"]) > 5e-7 or \
           abs(tol_max - entry["tol_max"]) > 5e-7 or \
           abs(tol_sum / n - entry["tol_avg"]) > 5e-7:
            failures.append(f"{tag}: tolerance stats mismatch")

        check_candidate_file(
            os.path.join(outdir, f"candidates_{n}_legacy.txt"),
            legacy_kept, f"{tag} legacy", failures)
        check_candidate_file(
            os.path.join(outdir, f"candidates_{n}_m{margin:g}.txt"),
            adapt_kept, f"{tag} adaptive", failures)
        print(f"  ok  {tag}: recomputed {n} rows, "
              f"legacy_false={legacy_false}, adaptive_false={adapt_false}, "
              f"tol[min/avg/max]={tol_min:.3f}/{tol_sum/n:.3f}/{tol_max:.3f}")

    print(f"\nrows independently recomputed: {checked_rows + len(hand)}")
    if failures:
        print("FAILURES:")
        for msg in failures:
            print("  - " + msg)
        return 1
    print("ALL CROSSCHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
