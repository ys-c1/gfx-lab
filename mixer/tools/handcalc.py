#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
电平表阻尼回落 —— 独立手算参考实现（与 C 代码互相独立，仅用标准库）。

物理模型（指针式表头阻尼）：
  1) 表头阻尼规定回落速度 v (dB/s)，线性幅度每秒衰减因子
        g = 10^(-v/20)
     每采样阻尼系数（第一段：由表头阻尼算回落速度/系数）
        alpha = g^(1/fs) = 10^(-v/(20*fs))
  2) 逐采样更新读数（第二段：按速度更新读数；攻击即时，回落受阻尼）
        target = |x|
        peak = target                       若 target >= peak（冲上去，无阻尼）
        peak = max(target, peak * alpha)    否则（按阻尼回落，不低于真实信号）
  动态范围 DR (dB) 对应下限 floor = 10^(-DR/20)。
  旧行为（v <= 0）：alpha = 0，读数立即跟随 |x|，块峰值等于朴素 max|x|。
"""
import csv
import math
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "artifacts")


def fall_alpha(fs, fall_db_per_s):
    """第一段：表头阻尼 -> 每采样阻尼系数。"""
    if fall_db_per_s <= 0.0:
        return 0.0  # 旧行为：无阻尼，立即回落
    return 10.0 ** (-fall_db_per_s / (20.0 * fs))


def floor_lin(dynamic_range_db):
    return 10.0 ** (-dynamic_range_db / 20.0)


def run(xs, fs, fall_db_per_s, dynamic_range_db):
    """第二段：按阻尼系数逐点更新读数，返回 (alpha, floor, readings)。"""
    a = fall_alpha(fs, fall_db_per_s)
    floor = floor_lin(dynamic_range_db)
    peak = 0.0
    rows = []
    for i, x in enumerate(xs):
        target = abs(x)
        if target >= peak:
            peak = target
        else:
            peak = max(target, peak * a)
        db = 20.0 * math.log10(peak) if peak > 0.0 else -math.inf
        rows.append((i, i / fs, target, peak, db))
    return a, floor, rows


def write_table(path, header, rows):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def small_case():
    """小图：fs=10 Hz、回落 20 dB/s、DR 60 dB；单位冲激后 14 个零点。"""
    fs, v, dr = 10, 20.0, 60.0
    xs = [1.0] + [0.0] * 14
    a, floor, rows = run(xs, fs, v, dr)
    return ("small", fs, v, dr, a, floor, rows)


def large_case():
    """大图：fs=48000 Hz、回落 33 dB/s、DR 80 dB；0.25s 恒幅 0.8 后 1.75s 静音。"""
    fs, v, dr = 48000, 33.0, 80.0
    n_tone = fs // 4          # 12000
    n = fs * 2                # 96000
    xs = [0.8] * n_tone + [0.0] * (n - n_tone)
    a, floor, rows = run(xs, fs, v, dr)
    return ("large", fs, v, dr, a, floor, rows)


def main():
    os.makedirs(OUT, exist_ok=True)
    summary = []
    golden = []
    for case in (small_case(), large_case()):
        name, fs, v, dr, a, floor, rows = case
        table = os.path.join(OUT, f"hand_{name}.csv")
        write_table(table, ["sample", "t_s", "target_lin", "peak_lin", "peak_db"],
                    [(r[0], f"{r[1]:.9f}", f"{r[2]:.9g}", f"{r[3]:.9g}",
                      f"{r[4]:.6f}" if math.isfinite(r[4]) else "-inf") for r in rows])
        # 黄金检查点（供 C 测试硬编码核对）
        gold = [r for r in rows if r[0] in (0, 1, 2, 3, 4, 5, 10, 14)] \
            if name == "small" else \
            [r for r in rows if r[0] in (0, 11999, 12000, 12001, 41090, 41091, 95999)]
        golden.append((name, fs, v, dr, a, floor, gold))
        summary.append((name, fs, v, dr, a, floor, len(rows), table))

    with open(os.path.join(OUT, "hand_summary.txt"), "w") as f:
        for name, fs, v, dr, a, floor, n, table in summary:
            f.write(f"[{name}] fs={fs} fall={v} dB/s DR={dr} dB "
                    f"alpha={a:.12g} floor={floor:.12g} rows={n} -> {table}\n")
        f.write("\n黄金检查点 (sample, t_s, target, peak_lin, peak_db):\n")
        for name, fs, v, dr, a, floor, gold in golden:
            f.write(f"-- {name} --\n")
            for r in gold:
                f.write(f"  k={r[0]:6d} t={r[1]:.9f}s target={r[2]:.9g} "
                        f"peak={r[3]:.12g} db={r[4]:.6f}\n")
    print(open(os.path.join(OUT, "hand_summary.txt")).read())


if __name__ == "__main__":
    main()
