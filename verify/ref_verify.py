#!/usr/bin/env python3
"""Independent reference for the C two-phase polygon fill (raster/polygon.c).

Algorithm is deliberately different in mechanism from the C code:
  * containment uses a horizontal ray cast implemented with exact rational
    arithmetic (fractions.Fraction), so every crossing sign is exact;
  * the C side mixes fan-triangle rasterization (ids) with a double-based
    isLeft ray. Here there is no fan and no floating point in the decision.

Run with a clean interpreter:  python3 -I verify.py <bench.json> [--strict]
It rebuilds the expected per-pixel parity/winding for every shape/grid in
bench.json, compares coverage/fill counts, and reports max pixel error and
timing for the 32 vs 256 grids.
"""
import argparse
import json
import math
import os
import sys
import time
from fractions import Fraction


def make_poly(shape, grid):
    """Mirror bench_poly.c vertex construction, but exactly."""
    if shape == "bowtie":
        s = Fraction(grid - 1, 15)
        base = [(2, 2), (14, 14), (14, 2), (2, 14)]
        return [(Fraction(x) * s + Fraction(1, 2),
                 Fraction(y) * s + Fraction(1, 2)) for x, y in base]
    # star
    c = Fraction(grid) / 2
    R = Fraction(grid) * Fraction(44, 100)
    r = Fraction(grid) * Fraction(19, 100)
    pts = []
    for i in range(10):
        rad = R if i % 2 == 0 else r
        ang = -math.pi / 2 + i * math.pi / 5
        # trig stays double (vertex coords), but containment below only
        # takes cross products in Fraction after rounding the vertex to the
        # same double the C side uses -> reconstruct via float then Fraction
        pts.append((Fraction(c + rad * math.cos(ang)),
                    Fraction(c + rad * math.sin(ang))))
    return pts


def point_state(verts, px, py):
    """Return (crossing_count, winding) for pixel centre (px,py).

    Half-open Sunday-style rules, exact Fraction arithmetic. Horizontal
    edges span no ray; a vertex on the ray is counted once.
    """
    wn = 0
    cr = 0
    n = len(verts)
    for k in range(n):
        ax, ay = verts[k]
        bx, by = verts[(k + 1) % n]
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


def reference_grid(verts, grid, timed=False):
    eo = nz = covered = 0
    states = {}
    cells = {}
    t0 = time.perf_counter()
    for y in range(grid):
        for x in range(grid):
            cr, wn = point_state(verts, Fraction(x) + Fraction(1, 2),
                                 Fraction(y) + Fraction(1, 2))
            cells[(x, y)] = (cr, wn)
            eo_in = (cr & 1) == 1
            nz_in = wn != 0
            if eo_in:
                eo += 1
            if nz_in:
                nz += 1
            if eo_in or nz_in:
                covered += 1
                key = (cr, wn)
                states[key] = states.get(key, 0) + 1
    dt = (time.perf_counter() - t0) * 1000.0 if timed else 0.0
    return {"eo": eo, "nz": nz, "covered": covered,
            "states": states, "ref_ms": dt, "cells": cells}


def verify_pixel_dumps(outdir, cache, strict):
    """Compare every dumped C pixel against the exact reference grid."""
    total_px = 0
    bad = 0
    dumps = sorted(f for f in os.listdir(outdir) if f.endswith("_cover.txt"))
    print(f"\nper-pixel cross-check against {len(dumps)} cover dump(s)")
    for name in dumps:
        shape, grid_s = name.split("_")[0], name.split("_")[1]
        grid = int(grid_s)
        ref = cache[(shape, grid)]["cells"]
        first_bad = []
        with open(os.path.join(outdir, name), encoding="utf-8") as f:
            for line in f:
                x, y, parity, winding, _ntris = map(int, line.split())
                exp_cr, exp_wn = ref[(x, y)]
                total_px += 1
                if (parity, winding) != (exp_cr, exp_wn):
                    bad += 1
                    if len(first_bad) < 5:
                        first_bad.append((x, y, (parity, winding),
                                          (exp_cr, exp_wn)))
        status = "ok" if not first_bad else "MISMATCH"
        print(f"  {name:24} grid={grid:4} {status}"
              + (f" first={first_bad}" if first_bad else ""))
    print(f"  compared {total_px} pixels, {bad} differ")
    if strict and bad:
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bench_json")
    ap.add_argument("--strict", action="store_true",
                    help="non-zero exit on any count mismatch")
    args = ap.parse_args()
    with open(args.bench_json, encoding="utf-8") as f:
        rows = json.load(f)

    # one reference per (shape, grid); compare against the OFF run which
    # always carries the full coverage stats regardless of shading rule
    cache = {}
    mismatches = 0
    print(f"{'shape':8} {'grid':>4} {'metric':10} {'C':>8} {'py-ref':>8} {'err':>5}")
    for r in rows:
        if r["mode"] != "off":
            continue
        key = (r["shape"], r["grid"])
        if key not in cache:
            verts = make_poly(r["shape"], r["grid"])
            cache[key] = reference_grid(verts, r["grid"], timed=True)
        ref = cache[key]
        for metric, cval, rval in (
            ("covered", r["covered"], ref["covered"]),
            ("eo_fill", r["eo_fill"], ref["eo"]),
            ("nz_fill", r["nz_fill"], ref["nz"]),
        ):
            err = cval - rval
            flag = "" if err == 0 else "  <-- MISMATCH"
            if err != 0:
                mismatches += 1
            print(f"{r['shape']:8} {r['grid']:>4} {metric:10} "
                  f"{cval:>8} {rval:>8} {err:>5}{flag}")

    # timing comparison 32 vs 256 (C best-of-N vs single python pass)
    print("\ntiming (ms): C best-of-iters vs python single exact pass")
    print(f"{'shape':8} {'grid':>4} {'C best':>10} {'python':>10} {'ratio':>8}")
    seen = set()
    for r in rows:
        if r["mode"] != "off":
            continue
        key = (r["shape"], r["grid"])
        if key in seen:
            continue
        seen.add(key)
        ref = cache[key]
        ratio = ref["ref_ms"] / r["best_ms"] if r["best_ms"] > 0 else float("inf")
        print(f"{r['shape']:8} {r['grid']:>4} {r['best_ms']:>10.4f} "
              f"{ref['ref_ms']:>10.4f} {ratio:>8.1f}x")

    # rule table cross-check: distinct states from C's nstates vs reference
    print("\nrule table state counts:")
    seen = set()
    for r in rows:
        if r["mode"] != "off":
            continue
        key = (r["shape"], r["grid"])
        if key in seen:
            continue
        seen.add(key)
        ref = cache[key]
        ok = "ok" if r["nstates"] == len(ref["states"]) else "MISMATCH"
        print(f"  {r['shape']:8} {r['grid']:>4} C nstates={r['nstates']} "
              f"ref states={len(ref['states'])}  {ok}  detail={sorted(ref['states'])}")

    print(f"\n{len(cache)} reference grids, {mismatches} mismatches")
    if args.strict and mismatches:
        sys.exit(1)

    verify_pixel_dumps(os.path.dirname(os.path.abspath(args.bench_json)),
                       cache, args.strict)


if __name__ == "__main__":
    main()
