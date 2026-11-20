#!/usr/bin/env python3
"""Per-sample reference implementation of the mixer look-ahead limiter.

Reads the raw float32 mixes dumped by benchmark.c, recomputes the limiter
sample by sample in pure Python (struct + math, float32 emulation through
ctypes.c_float), and compares against the C output bit for bit / value for
value. Also reports plateau runs and peaks independently.
"""
import ctypes
import math
import os
import struct
import sys

CEIL_MARGIN = 2.0e-4
PLATEAU_EPS = 1e-7


def f32(x):
    return ctypes.c_float(x).value


def read_f32(path):
    with open(path, "rb") as f:
        raw = f.read()
    return list(struct.unpack("<%df" % (len(raw) // 4), raw))


def limit_channel(x, n, threshold, sr, attack_ms, release_ms):
    """Two-pass look-ahead brick-wall limiter for one channel."""
    ceil_v = f32(threshold * f32(1.0 - CEIL_MARGIN))
    look = max(1, int(round(attack_ms * 1e-3 * sr)))
    rel_coef = f32(math.exp(f32(-1.0 / f32(release_ms * 1e-3 * sr))))

    g = [0.0] * n
    # forward pass: local peak inside [i, i+look) -> required gain
    for i in range(n):
        pk = 0.0
        for j in range(i, min(i + look, n)):
            v = abs(x[j])
            if v > pk:
                pk = v
        g[i] = f32(ceil_v / pk) if pk > ceil_v else 1.0
    # backward pass: release-rate smoothing
    env = 1.0
    for i in range(n - 1, -1, -1):
        if g[i] < env:
            env = g[i]
        else:
            env = f32(f32(rel_coef * env) + f32(f32(1.0 - rel_coef) * g[i]))
        g[i] = env
    # apply
    y = [0.0] * n
    limited = 0
    for i in range(n):
        xi = x[i]
        if not math.isfinite(xi):
            xi = 0.0
        if g[i] < 1.0:
            limited += 1
        y[i] = f32(xi * g[i])
    return y, g, limited


def plateau_stats(y, threshold):
    runs, samples, run = 0, 0, 0
    for v in y:
        if abs(v) >= threshold - PLATEAU_EPS:
            run += 1
        else:
            if run >= 2:
                runs += 1
                samples += run
            run = 0
    if run >= 2:
        runs += 1
        samples += run
    return runs, samples


def main(build_dir, frames, channels, threshold, sr, attack_ms, release_ms):
    rc_all = 0
    for tracks in (16, 64):
        raw_in = read_f32(os.path.join(build_dir, "mix_%d_in.f32" % tracks))
        c_out = read_f32(os.path.join(build_dir, "mix_%d_out.f32" % tracks))
        n = frames
        py_out = [0.0] * (n * channels)
        limited_total = 0
        for c in range(channels):
            x = [raw_in[i * channels + c] for i in range(n)]
            y, _g, lim = limit_channel(x, n, threshold, sr, attack_ms, release_ms)
            limited_total += lim
            for i, v in enumerate(y):
                py_out[i * channels + c] = v

        # per-sample comparison vs the C implementation
        max_abs = 0.0
        bit_mismatch = 0
        for a, b in zip(py_out, c_out):
            d = abs(a - b)
            if d > max_abs:
                max_abs = d
            if struct.pack("<f", a) != struct.pack("<f", b):
                bit_mismatch += 1

        flat_in = raw_in
        peak_before = max(abs(v) for v in flat_in)
        peak_after_py = max(abs(v) for v in py_out)
        peak_after_c = max(abs(v) for v in c_out)
        py_runs, py_plateau = plateau_stats(py_out, threshold)
        c_runs, c_plateau = plateau_stats(c_out, threshold)
        guard_ok = all(abs(v) <= threshold for v in c_out)

        print("tracks=%d" % tracks)
        print("  peak: before=%.6f  after(py)=%.6f  after(C)=%.6f"
              % (peak_before, peak_after_py, peak_after_c))
        print("  peak error vs threshold: over=%.3g  headroom=%.6f  "
              "C-vs-hand=%.3g"
              % (max(0.0, peak_after_c - threshold),
                 threshold - peak_after_c,
                 abs(peak_after_c - peak_after_py)))
        print("  gain reduction: %.4f dB"
              % (20.0 * math.log10(peak_after_c / peak_before)))
        print("  plateau: runs py=%d C=%d  samples py=%d C=%d  (limited=%d)"
              % (py_runs, c_runs, py_plateau, c_plateau, limited_total))
        print("  guard |y|<=threshold: %s" % guard_ok)
        print("  per-sample vs hand calc: max_abs_diff=%.3g  bit_mismatches=%d/%d"
              % (max_abs, bit_mismatch, len(c_out)))
        if not guard_ok or bit_mismatch != 0 or max_abs > 0.0:
            rc_all = 1
    return rc_all


if __name__ == "__main__":
    build = sys.argv[1] if len(sys.argv) > 1 else "build"
    meta = {}
    with open(os.path.join(build, "bench_meta.txt")) as f:
        for tok in f.read().split():
            k, v = tok.split("=")
            meta[k] = float(v) if "." in v else int(v)
    sys.exit(main(build, meta["frames"], meta["channels"], meta["threshold"],
                  meta["sample_rate"], meta["attack_ms"], meta["release_ms"]))
