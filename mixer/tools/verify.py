#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
核对器：三层模型逐行核对，小图/大图各一遍。

  理想模型 hand  : double 精度，物理真值（与 handcalc.py 同一公式）
  f32 模型 f32   : 每一步运算都强制舍入到 IEEE-754 binary32，模拟 C 的 float 运算
  C 实现         : 实际编译运行的 meter.c，导出曲线 CSV

核对关系：
  C   vs f32  —— 实现正确性，应只相差数个 ulp
  f32 vs hand —— float32 系数量化/连乘累积带来的物理误差（信息性，给上界）
回落时间同样分解：
  量化+系数漂移误差 = f32 模型首穿点 vs 理想连续时间
  实现误差          = C 首穿点 vs f32 首穿点（应为 0 采样）
仅依赖标准库；以 -I 运行，TMPDIR 指向工作区，避免占满 /tmp。
"""
import csv
import math
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MIX = os.path.join(ROOT, "mixer")
ART = os.path.join(ROOT, "artifacts")
TMP = os.path.join(ART, "tmp")
EPS32 = 2.0 ** -24  # binary32 机器 epsilon ≈ 5.96e-8
ULP_TOL_REL = 4.0 * EPS32   # C vs f32 模型：容 4 个 ulp
ULP_TOL_ABS = 1e-7


def f32(x):
    """把一个 double 强制舍入为 binary32 再转回 double。"""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def hand_curve(xs, fs, fall):
    """理想 double 模型。返回 dict: sample -> (target, peak, db) 与 alpha。"""
    alpha = 0.0 if fall <= 0 else 10.0 ** (-fall / (20.0 * fs))
    peak, out = 0.0, {}
    for i, x in enumerate(xs):
        target = abs(x)
        peak = target if target >= peak else max(target, peak * alpha)
        db = 20.0 * math.log10(peak) if peak > 0 else -math.inf
        out[i] = (target, peak, db)
    return out, alpha


def f32_curve(xs, fs, fall):
    """binary32 仿真模型：每个中间量都按 C 的 float 语义舍入。"""
    if fall <= 0:
        alpha32 = 0.0
    else:
        alpha32 = f32(10.0 ** f32(-fall / f32(20.0 * fs)))
    peak, out = 0.0, {}
    for i, x in enumerate(xs):
        target = f32(abs(x))
        if target >= peak:
            peak = target
        else:
            damped = f32(peak * alpha32)
            peak = target if target > damped else damped
        db = 20.0 * math.log10(peak) if peak > 0 else -math.inf
        out[i] = (target, peak, db)
    return out, alpha32


def crossing_sample(rows, start, start_peak, drop_db):
    thr = start_peak * 10.0 ** (-drop_db / 20.0)
    for k in sorted(rows):
        if k >= start and rows[k][1] <= thr:
            return k
    return -1


def read_c_curve(path):
    rows = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            k = int(r["sample"])
            db = float(r["peak_db"]) if r["peak_db"] != "-inf" else -math.inf
            rows[k] = (float(r["target_lin"]), float(r["peak_lin"]), db)
    return rows


def parse_bench(text):
    cases, cur = {}, None
    for line in text.splitlines():
        if line.startswith("CASE "):
            cur = line.split()[1]; cases[cur] = {}
        elif cur and " " in line:
            k, v = line.split(" ", 1)
            if k in ("CURVE", "EMPTY_ERR", "NULL_ERR"):
                cases[cur][k] = v
            else:
                try:
                    fv = float(v)
                    cases[cur][k] = int(fv) if fv == int(fv) and "." not in v else fv
                except ValueError:
                    cases[cur][k] = v
    return cases


def signal_spec(name):
    if name == "small":
        fs, fall, dr, n, tone, amp = 10, 20.0, 60.0, 15, 1, 1.0
    else:
        fs, fall, dr, n, tone, amp = 48000, 33.0, 80.0, 96000, 12000, 0.8
    xs = [amp] * tone + [0.0] * (n - tone)
    return fs, fall, dr, n, tone, amp, xs


def curve_diff(a, b):
    """返回 a 相对 b 的逐点误差统计。"""
    max_abs, max_rel, max_db, rms_n = 0.0, 0.0, 0.0, 0.0
    rms_acc, rms_db, bad = 0.0, 0.0, 0
    for k, (_, pb, db_b) in b.items():
        pa, db_a = a[k][1], a[k][2]
        e = abs(pa - pb)
        rel = e / pb if pb > 0 else e
        ed = abs(db_a - db_b) if math.isfinite(db_b) and math.isfinite(db_a) else 0.0
        max_abs = max(max_abs, e); max_rel = max(max_rel, rel); max_db = max(max_db, ed)
        rms_acc += e * e; rms_db += ed * ed; rms_n += 1
        if e > ULP_TOL_ABS + ULP_TOL_REL * pb:
            bad += 1
    return max_abs, max_rel, max_db, math.sqrt(rms_acc / rms_n), math.sqrt(rms_db / rms_n), bad


def main():
    os.makedirs(ART, exist_ok=True)
    os.makedirs(TMP, exist_ok=True)
    env = dict(os.environ, TMPDIR=TMP)

    make_test = subprocess.run(["make", "-C", MIX, "test"], env=env,
                               capture_output=True, text=True)
    test_log = make_test.stdout + make_test.stderr
    open(os.path.join(ART, "c_test.log"), "w", encoding="utf-8").write(test_log)
    test_ok = make_test.returncode == 0

    make_bench = subprocess.run(["make", "-C", MIX, "bench"], env=env,
                                capture_output=True, text=True)
    if make_bench.returncode != 0:
        print(make_bench.stdout + make_bench.stderr)
        return 2
    bench = subprocess.run([os.path.join(MIX, "build", "bench")], cwd=MIX,
                           env=env, capture_output=True, text=True)
    open(os.path.join(ART, "bench.log"), "w", encoding="utf-8").write(
        bench.stdout + bench.stderr)
    if bench.returncode != 0:
        print(bench.stdout + bench.stderr)
        return 2
    meta = parse_bench(bench.stdout)

    report = ["电平表阻尼回落 — 核对报告", "=" * 64,
              f"C 单元测试: {'PASS' if test_ok else 'FAIL'}", ""]
    all_ok = [test_ok]

    for name in ("small", "large"):
        fs, fall, dr, n, tone, amp, xs = signal_spec(name)
        hand, alpha_ideal = hand_curve(xs, fs, fall)
        f32m, alpha32 = f32_curve(xs, fs, fall)
        cpath = os.path.join(MIX, meta[name]["CURVE"])
        crows = read_c_curve(cpath)
        copy_dst = os.path.join(ART, f"curve_{name}.csv")
        with open(cpath, encoding="utf-8") as src, open(copy_dst, "w", encoding="utf-8") as dst:
            dst.write(src.read())

        assert set(crows) == set(hand) == set(f32m)
        # C vs f32 模型：实现正确性
        c_abs, c_rel, c_db, c_rms, c_rmsdb, c_bad = curve_diff(crows, f32m)
        impl_ok = c_bad == 0
        # f32 模型 vs 理想 double：float32 物理误差
        f_abs, f_rel, f_db, f_rms, f_rmsdb, _ = curve_diff(f32m, hand)
        phys_bound = n * EPS32 * 2.0   # 连乘 n 步的宽松相对上界
        phys_ok = f_rel <= phys_bound

        # 回落 20dB：三层首穿点；基准为最后一个持峰采样 tone-1
        base = tone - 1
        k_hand = crossing_sample(hand, base, amp, 20.0)
        k_f32 = crossing_sample(f32m, base, amp, 20.0)
        k_c = meta[name]["FALL_CROSS_SAMPLES"] + base
        steps = k_f32 - base
        t_ideal = 20.0 / fall
        t_f32 = steps / fs
        t_c = (k_c - base) / fs
        quant_err = t_f32 - t_ideal
        impl_err_samples = k_c - k_f32
        # 解析上界：1 个采样的离散量化 + alpha 量化经 steps 次连乘的漂移。
        # 电平相对误差 ≈ steps*|Δalpha|/alpha；换算成采样偏移再除以每采样
        # 对数衰减率 |ln alpha| = v*ln10/(20*fs)。
        alpha_rel = abs(alpha32 - alpha_ideal) / alpha_ideal
        rate = fall * math.log(10.0) / (20.0 * fs)   # 每采样 |ln alpha|
        bound_samples = 1.0 + steps * alpha_rel / rate
        quant_ok = abs(quant_err * fs) <= bound_samples

        m = meta[name]
        all_ok += [impl_ok, phys_ok, impl_err_samples == 0, quant_ok]

        label = "小图" if name == "small" else "大图"
        report += [
            f"【{label}】{name}",
            f"  采样率 fs              : {fs} Hz，采样点 {n}",
            f"  阻尼/回落速度 v        : {fall:.3f} dB/s",
            f"  动态范围 DR            : {dr:.1f} dB (线性下限 {10**(-dr/20):.3g})",
            f"  阻尼系数 alpha 理想    : {alpha_ideal:.12g}",
            f"  阻尼系数 alpha(f32/C)  : {alpha32:.12g} / {m['ALPHA']:.12g}",
            f"  alpha 量化误差         : {abs(m['ALPHA'] - alpha_ideal):.3e} (相对 {abs(m['ALPHA']-alpha_ideal)/alpha_ideal:.3e})",
            "",
            f"  [实现核对] C vs f32 模型（容差 {ULP_TOL_REL:.2e} 相对 + {ULP_TOL_ABS:.0e} 绝对）",
            f"    最大绝对误差         : {c_abs:.3e}",
            f"    最大相对误差         : {c_rel:.3e} ({c_rel/EPS32:.2f} ulp)",
            f"    最大 dB 误差         : {c_db:.3e} dB",
            f"    RMS 绝对/dB 误差     : {c_rms:.3e} / {c_rmsdb:.3e} dB",
            f"    超容差点数           : {c_bad} / {n}  -> {'PASS' if impl_ok else 'FAIL'}",
            "",
            f"  [物理误差] f32 模型 vs double 理想（float32 连乘累积，信息性）",
            f"    最大绝对误差         : {f_abs:.3e}",
            f"    最大相对误差         : {f_rel:.3e} (上界 {phys_bound:.3e})",
            f"    最大 dB 误差         : {f_db:.3e} dB",
            f"    RMS 绝对/dB 误差     : {f_rms:.3e} / {f_rmsdb:.3e} dB",
            f"    上界核对             : {'PASS' if phys_ok else 'FAIL'}",
            "",
            f"  处理耗时               : {m['PROCESS_NS']:.1f} ns 总计, {m['NS_PER_SAMPLE']:.3f} ns/采样",
            f"  回落 20dB 理想时间     : {t_ideal:.6f} s (=20/{fall:g})",
            f"  回落 20dB f32 首穿     : 间隔 {steps} 采样，{t_f32:.6f} s",
            f"  回落 20dB C 首穿       : 间隔 {k_c-base} 采样，{t_c:.6f} s",
            f"  量化+系数漂移误差      : {quant_err*1000:.4f} ms ({quant_err*fs:+.3f} 采样)",
            f"  解析误差上界           : {bound_samples:.3f} 采样 -> {'PASS' if quant_ok else 'FAIL'}",
            f"  实现误差(C-f32)        : {impl_err_samples} 采样 -> {'PASS' if impl_err_samples == 0 else 'FAIL'}",
            f"  曲线导出               : {copy_dst}（{m['CURVE_ROWS']} 行）",
            f"  空输入错误             : {m['EMPTY_ERR']}",
            f"  空指针错误             : {m['NULL_ERR']}",
            ""]

    report.append("总结: " + ("全部 PASS" if all(all_ok) else "存在 FAIL"))
    text = "\n".join(report)
    open(os.path.join(ART, "verify_report.txt"), "w", encoding="utf-8").write(text + "\n")
    print(text)
    return 0 if all(all_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
