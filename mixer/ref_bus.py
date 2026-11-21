#!/usr/bin/env python3
"""Independent cross-check for the two-stage per-bus compressor.

Reads the raw float32 dumps and the CSV table written by bench.c, then:

  1. for the hand fixture, compares build/buses_hand.csv with the checked-in
     mixer/hand_table.csv cell by cell (every column, every bus);
  2. for the small/large benchmark scales, re-reads each bus's input dump,
     recomputes level, gain, peaks, RMS, loudness delta and compressed
     sample count from scratch (double precision, float32 rounding where the
     C side stores float), and compares them with the C report row;
  3. re-applies the C gain to the input dump to verify the output dump is
     exactly in*gain sample by sample.

Exit status is non-zero on any mismatch.
"""

import csv
import math
import os
import struct
import sys

F32 = struct.Struct("<f")

# Columns whose stored value is a float and should match to float precision.
FLOAT_COLS = {
    "level", "gain", "gain_db", "reduction_db", "peak_before", "peak_after",
    "rms_before", "rms_after", "loudness_before_dbfs", "loudness_after_dbfs",
    "loudness_delta_db", "threshold", "ratio",
}
INT_COLS = {
    "bus", "enabled", "samples_over_threshold", "compressed_samples",
    "samples", "frames", "channels",
}


def f32(x):
    return F32.unpack(F32.pack(x))[0]


def read_f32(path):
    with open(path, "rb") as f:
        raw = f.read()
    return list(struct.unpack("<%df" % (len(raw) // 4), raw))


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def read_meta(path):
    frames = channels = buses = None
    cfgs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("bus="):
                b, det, thr, ratio, en = line[4:].split(",")
                cfgs.append({
                    "bus": int(b), "detector": det,
                    "threshold": f32(float(thr)), "ratio": f32(float(ratio)),
                    "enabled": int(en),
                })
            else:
                k, v = line.split("=")
                if k == "frames":
                    frames = int(v)
                elif k == "channels":
                    channels = int(v)
                elif k == "buses":
                    buses = int(v)
    return frames, channels, buses, cfgs


def measure(samples, detector):
    n = len(samples)
    peak = 0.0
    ss = 0.0
    for v in samples:
        a = abs(v)
        if a > peak:
            peak = a
        ss += v * v
    peak = f32(peak)
    rms = f32(math.sqrt(ss / n))
    return peak, rms, (rms if detector == "rms" else peak)


def db(x):
    return 20.0 * math.log10(x) if x > 0.0 else float("-inf")


def compare_float(col, got, want, tol, where, problems):
    if math.isinf(want):
        if not math.isinf(got) or (got > 0) != (want > 0):
            problems.append("%s %s: %r vs %r" % (where, col, got, want))
        return
    if not (abs(got - want) <= tol * (1.0 + abs(want))):
        problems.append("%s %s: %.10g vs %.10g (err %.3g)"
                        % (where, col, got, want, abs(got - want)))


def check_hand_table(hand_path, build_csv, problems):
    ref = read_csv(hand_path)
    got = read_csv(build_csv)
    if len(ref) != len(got):
        problems.append("hand table row count %d vs %d" % (len(ref), len(got)))
        return
    for r_row, g_row in zip(ref, got):
        for col in r_row:
            if col not in g_row:
                problems.append("hand table missing column %s" % col)
                continue
            where = "hand bus %s" % r_row["bus"]
            if col in FLOAT_COLS:
                compare_float(col, float(g_row[col]), float(r_row[col]),
                              1e-6, where, problems)
            elif col in INT_COLS:
                if int(g_row[col]) != int(r_row[col]):
                    problems.append("%s %s: %s vs %s"
                                    % (where, col, g_row[col], r_row[col]))
            elif col == "detector":
                if g_row[col] != r_row[col]:
                    problems.append("%s detector: %s vs %s"
                                    % (where, g_row[col], r_row[col]))
            else:
                problems.append("hand table unknown column %s" % col)
    print("hand table: %d rows x %d columns checked against %s"
          % (len(ref), len(ref[0]) if ref else 0, hand_path))


def check_scale(scale, build_dir, problems):
    meta_path = os.path.join(build_dir, "bench_%s.meta" % scale)
    frames, channels, buses, cfgs = read_meta(meta_path)
    ref = read_csv(os.path.join(build_dir, "buses_%s.csv" % scale))
    if len(ref) != buses:
        problems.append("%s: csv has %d rows, expected %d"
                        % (scale, len(ref), buses))
        return
    print("[%s] %d frames x %d ch x %d buses: independently recomputing table"
          % (scale, frames, channels, buses))
    worst = {"level": 0.0, "gain": 0.0, "loud": 0.0, "peak": 0.0}
    for b in range(buses):
        cfg = cfgs[b]
        row = ref[b]
        if int(row["bus"]) != b:
            problems.append("%s: row %d has bus %s" % (scale, b, row["bus"]))
        if row["detector"] != cfg["detector"]:
            problems.append("%s bus %d: detector %s vs %s"
                            % (scale, b, row["detector"], cfg["detector"]))

        in_path = os.path.join(build_dir, "bench_%s_bus%d_in.f32" % (scale, b))
        out_path = os.path.join(build_dir, "bench_%s_bus%d_out.f32" % (scale, b))
        inp = read_f32(in_path)
        out = read_f32(out_path)
        assert len(inp) == frames * channels, "input dump length"

        peak_b, rms_b, level = measure(inp, cfg["detector"])
        thr, ratio = cfg["threshold"], cfg["ratio"]
        gain = (f32(math.pow(level / thr, 1.0 / ratio - 1.0))
                if (cfg["enabled"] and ratio > 1.0 and level > thr) else f32(1.0))

        # Re-apply the reported gain to the input; must equal the output dump.
        mismatches = 0
        for a, c in zip(inp, out):
            if f32(a * gain) != c:
                mismatches += 1
        if mismatches:
            problems.append("%s bus %d: %d output samples differ from in*recomputed gain"
                            % (scale, b, mismatches))

        # Also check the reported gain itself was applied.
        rep_gain = f32(float(row["gain"]))
        mismatches_c = sum(1 for a, c in zip(inp, out) if f32(a * rep_gain) != c)
        if mismatches_c:
            problems.append("%s bus %d: %d output samples differ from in*reported gain"
                            % (scale, b, mismatches_c))

        out_ss = sum(v * v for v in out)
        rms_a = f32(math.sqrt(out_ss / len(out)))
        peak_a = f32(max(abs(v) for v in out)) if out else 0.0
        comp = len(inp) if gain < 1.0 else 0
        over = sum(1 for v in inp if abs(v) > thr)

        compare_float("level", float(row["level"]), level, 1e-6,
                      "%s bus %d" % (scale, b), problems)
        compare_float("gain", float(row["gain"]), gain, 1e-6,
                      "%s bus %d" % (scale, b), problems)
        compare_float("gain_db", float(row["gain_db"]),
                      f32(db(gain)) if gain < 1.0 else 0.0, 1e-5,
                      "%s bus %d" % (scale, b), problems)
        compare_float("peak_before", float(row["peak_before"]), peak_b, 1e-6,
                      "%s bus %d" % (scale, b), problems)
        compare_float("peak_after", float(row["peak_after"]), peak_a, 1e-6,
                      "%s bus %d" % (scale, b), problems)
        compare_float("rms_before", float(row["rms_before"]), rms_b, 1e-6,
                      "%s bus %d" % (scale, b), problems)
        compare_float("rms_after", float(row["rms_after"]), rms_a, 1e-6,
                      "%s bus %d" % (scale, b), problems)
        loud_delta = f32(db(rms_a) - db(rms_b))
        compare_float("loudness_delta_db", float(row["loudness_delta_db"]),
                      loud_delta, 1e-4, "%s bus %d" % (scale, b), problems)
        if int(row["compressed_samples"]) != comp:
            problems.append("%s bus %d: compressed %s vs %d"
                            % (scale, b, row["compressed_samples"], comp))
        if int(row["samples_over_threshold"]) != over:
            problems.append("%s bus %d: over-threshold %s vs %d"
                            % (scale, b, row["samples_over_threshold"], over))
        if int(row["samples"]) != len(inp):
            problems.append("%s bus %d: samples %s vs %d"
                            % (scale, b, row["samples"], len(inp)))

        worst["level"] = max(worst["level"], abs(float(row["level"]) - level))
        worst["gain"] = max(worst["gain"], abs(float(row["gain"]) - gain))
        worst["loud"] = max(worst["loud"],
                            abs(float(row["loudness_delta_db"]) - loud_delta))
        worst["peak"] = max(worst["peak"],
                            abs(float(row["peak_after"]) - peak_a))
    print("  max abs error: level %.3g, gain %.3g, loudness_delta %.3g dB, "
          "peak_after %.3g" % (worst["level"], worst["gain"], worst["loud"],
                               worst["peak"]))


def main(argv):
    build_dir = argv[1] if len(argv) > 1 else "build"
    hand_ref = argv[2] if len(argv) > 2 else "hand_table.csv"
    problems = []

    hand_build = os.path.join(build_dir, "buses_hand.csv")
    if os.path.exists(hand_build) and os.path.exists(hand_ref):
        check_hand_table(hand_ref, hand_build, problems)
    else:
        problems.append("missing hand table: %s or %s" % (hand_ref, hand_build))

    for scale in ("small", "large"):
        if os.path.exists(os.path.join(build_dir, "buses_%s.csv" % scale)):
            check_scale(scale, build_dir, problems)
        else:
            problems.append("missing %s scale output in %s" % (scale, build_dir))

    if problems:
        print("\n%d mismatch(es):" % len(problems))
        for p in problems[:60]:
            print("  " + p)
        if len(problems) > 60:
            print("  ... %d more" % (len(problems) - 60))
        return 1
    print("\nall cross-checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
