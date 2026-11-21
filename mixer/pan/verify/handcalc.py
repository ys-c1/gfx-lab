#!/usr/bin/env python3
"""独立手算对照：不 import 项目代码，只用 math/csv 复算声像增益与位置，
并把结果和 C 侧导出的 build/pan_metrics.csv、build/pan_diff.csv 逐项比较。
运行方式: python3 -I handcalc.py [build 目录]
"""
import csv
import math
import os
import sys

PI = math.pi
NLAW = ("linear", "equal-power")


def gains(law, p):
    """解析解：返回 (gL, gR)。p ∈ [-1, +1]。"""
    if law == "linear":
        return 0.5 * (1.0 - p), 0.5 * (1.0 + p)
    theta = (p + 1.0) * (PI / 4.0)
    return math.cos(theta), math.sin(theta)


def position(law, gl, gr):
    """第一段的逆运算：由增益反解位置。"""
    if law == "linear":
        return (gr - gl) / (gl + gr)
    theta = math.atan2(gr, gl)
    return 4.0 * theta / PI - 1.0


def read_csv(path):
    with open(path, newline="") as fh:
        rows = [r for r in csv.reader(fh) if r and not r[0].startswith("#")]
    head, body = rows[0], rows[1:]
    return head, [dict(zip(head, r)) for r in body]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, os.pardir, "build")
    metrics = os.path.join(build, "pan_metrics.csv")
    diff = os.path.join(build, "pan_diff.csv")
    if not os.path.exists(metrics) or not os.path.exists(diff):
        print("missing %s / %s; run `make -C mixer pan-bench` first" % (metrics, diff))
        return 2

    # ---- 1. 解析自检：恒功率 / 恒和 / 端点重合 / 中点 +3.0103 dB ----
    for i in range(-10, 11):
        p = i / 10.0
        gl, gr = gains("equal-power", p)
        assert abs(gl * gl + gr * gr - 1.0) < 1e-12, (p, gl, gr)
        for g in (gl, gr):
            assert -1e-12 <= g <= 1.0 + 1e-12
        ll, lr = gains("linear", p)
        assert abs(ll + lr - 1.0) < 1e-12
        for law, g in (("equal-power", (gl, gr)), ("linear", (ll, lr))):
            back = position(law, *g)
            assert abs(back - p) < 1e-12, (law, p, back)
    for p in (-1.0, 1.0):
        a = gains("equal-power", p)
        b = gains("linear", p)
        assert all(abs(x - y) < 1e-12 for x, y in zip(a, b)), (p, a, b)
    gl, gr = gains("equal-power", 0.0)
    ll, lr = gains("linear", 0.0)
    mid_db = 10.0 * math.log10((gl * gl + gr * gr) / (ll * ll + lr * lr))
    assert abs(mid_db - 3.0102999566) < 1e-9, mid_db
    print("解析自检通过: 恒功率、恒和、端点重合、中点 +%.4f dB" % mid_db)

    # ---- 2. 与 C 侧逐行对照 ----
    head, rows = read_csv(metrics)
    assert "gain_max_err" in head and "pos_est" in head
    worst_gain = worst_energy = worst_pos = worst_sample = 0.0
    n = 0
    for r in rows:
        law, ch, p = r["law"], int(r["channels"]), float(r["position"])
        gl, gr = gains(law, p)
        assert abs(float(r["gL_ana"]) - gl) < 5e-7, (law, p, r["gL_ana"], gl)
        assert abs(float(r["gR_ana"]) - gr) < 5e-7
        assert abs(float(r["gL_meas"]) - gl) < 2e-6, (law, ch, p, r["gL_meas"], gl)
        assert abs(float(r["gR_meas"]) - gr) < 2e-6
        assert abs(float(r["energy_ana"]) - (gl * gl + gr * gr)) < 1e-6
        back = position(law, float(r["gL_meas"]), float(r["gR_meas"]))
        assert abs(back - p) < 2e-5, (law, ch, p, back)
        assert abs(float(r["pos_est"]) - p) < 2e-5
        assert int(r["pairs"]) == ch // 2, (ch, r["pairs"])
        assert int(r["frames"]) > 0
        assert float(r["sample_peak"]) <= 1.0
        worst_gain = max(worst_gain, float(r["gain_max_err"]))
        worst_energy = max(worst_energy, float(r["energy_err"]))
        worst_pos = max(worst_pos, float(r["pos_err"]))
        worst_sample = max(worst_sample, float(r["sample_max_err"]))
        n += 1
    print("%d 行指标对照通过: 最大误差 增益 %.2e / 能量 %.2e / 位置 %.2e / 样本 %.2e"
          % (n, worst_gain, worst_energy, worst_pos, worst_sample))

    # ---- 3. 两律差异逐项对照 ----
    head, rows = read_csv(diff)
    assert {"dgL", "dgR", "energy_ep", "level_gain_db", "pos_linear_reads_ep",
            "output_max_diff"} <= set(head)
    for r in rows:
        p = float(r["position"])
        ll, lr = gains("linear", p)
        el, er = gains("equal-power", p)
        assert abs(float(r["gL_linear"]) - ll) < 5e-7
        assert abs(float(r["gL_ep"]) - el) < 5e-7
        assert abs(float(r["dgL"]) - (el - ll)) < 1e-6
        assert abs(float(r["dgR"]) - (er - lr)) < 1e-6
        assert abs(float(r["energy_ep"]) - 1.0) < 1e-6
        level = 10.0 * math.log10((el * el + er * er) / (ll * ll + lr * lr))
        assert abs(float(r["level_gain_db"]) - level) < 1e-3, (p, r["level_gain_db"], level)
        read = position("linear", el, er)
        assert abs(float(r["pos_linear_reads_ep"]) - read) < 1e-5
        # 读偏差 = |按线性律反解等功率增益得到的位置 - 请求位置|
        assert abs(float(r["output_max_diff"]) >= 0.0)
    # 差异的定性规律：端点为 0、中点电平差最大、两律在两端的增益完全相同
    endpoints = [r for r in rows if abs(abs(float(r["position"])) - 1.0) < 1e-9]
    for r in endpoints:
        # 端点两律解析上重合；cos(pi/2) 在 double 下留 6.1e-17 的残差
        assert abs(float(r["dgL"])) < 1e-12 and abs(float(r["dgR"])) < 1e-12
        assert abs(float(r["level_gain_db"])) < 1e-9
    mid = [r for r in rows if abs(float(r["position"])) < 1e-9][0]
    assert abs(float(mid["level_gain_db"]) - 3.0103) < 1e-3
    print("两律差异对照通过: %d 个位置，端点重合、中点电平差 %.4f dB"
          % (len(rows), float(mid["level_gain_db"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
