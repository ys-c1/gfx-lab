#!/usr/bin/env python3
"""独立手算对照：不 import 项目代码，只用 math/csv 复算系数与幅频响应，
并与 demo 导出的 demo_coef.csv 逐项比较。运行方式: python3 -I handcalc.py [coef.csv]
"""
import csv
import math
import os
import sys

SR = 48000
F_LO, F_HI = 250.0, 4000.0
FREQS = (80.0, 1000.0, 10000.0)
GAINS = (0.75, 1.5, 0.5)  # 与 demo 第一组一致


def lpf_coef(fc, sr):
    w = math.tan(math.pi * fc / sr)
    a1 = (w - 1.0) / (w + 1.0)
    b0 = 0.5 * (1.0 + a1)
    return b0, b0, a1


def hpf_coef(lp):
    b0, b1, a1 = lp
    # 高通 = 1 - H_lp：b0hp=1-b0, b1hp=a1-b1，分母相同
    return 1.0 - b0, a1 - b1, a1


def mag(cf, f, sr):
    return abs(H(cf, f, sr))


def H(cf, f, sr):
    """单节复频响 H(e^jw)"""
    b0, b1, a1 = cf
    w = 2.0 * math.pi * f / sr
    e = complex(math.cos(-w), math.sin(-w))
    return (b0 + b1 * e) / (1.0 + a1 * e)


def total_response(hand, f, sr, gains):
    """按真实信号流复域求和：low=L_lo; high=H_lo; mid=L_hi*high; treble=H_hi*high"""
    glo, gmid, ghi = gains
    low = H(hand["lp_lo"], f, sr)
    high = H(hand["hp_lo"], f, sr)
    mid = H(hand["lp_hi"], f, sr) * high
    tre = H(hand["hp_hi"], f, sr) * high
    return glo * low + gmid * mid + ghi * tre


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    csv_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, os.pardir, "demo_coef.csv")

    hand = {
        "lp_lo": lpf_coef(F_LO, SR),
        "hp_lo": hpf_coef(lpf_coef(F_LO, SR)),
        "lp_hi": lpf_coef(F_HI, SR),
        "hp_hi": hpf_coef(lpf_coef(F_HI, SR)),
    }
    print("手算系数 (b0, b1, a1):")
    for name, cf in hand.items():
        print(f"  {name:5s}: {cf[0]:.9g} {cf[1]:.9g} {cf[2]:.9g}")

    # 截止频点必为 -3.01 dB；互补两节幅频和为 1
    m_cut = mag(hand["lp_lo"], F_LO, SR)
    assert abs(m_cut - 1.0 / math.sqrt(2.0)) < 1e-6, m_cut
    for f in [20.0, 80.0, 250.0, 1000.0, 4000.0, 10000.0, 20000.0]:
        # 功率互补：|LP|^2 + |HP|^2 = 1（复域 H_lp+H_hp=1 推出）
        s1 = mag(hand["lp_lo"], f, SR) ** 2 + mag(hand["hp_lo"], f, SR) ** 2
        s2 = mag(hand["lp_hi"], f, SR) ** 2 + mag(hand["hp_hi"], f, SR) ** 2
        assert abs(s1 - 1.0) < 1e-9 and abs(s2 - 1.0) < 1e-9, (f, s1, s2)
    print("功率互补检查通过: |LP|^2+|HP|^2 = 1（7 个频点，误差 < 1e-9）")

    # 与 C 侧导出文件逐项比较
    if os.path.exists(csv_path):
        with open(csv_path, newline="") as fh:
            rows = [r for r in csv.reader(fh) if r and not r[0].startswith("#")]
        max_err = 0.0
        for r in rows:
            _, role, _fc, b0, b1, a1 = r
            exp = hand[role]
            got = tuple(float(x) for x in (b0, b1, a1))
            err = max(abs(a - b) for a, b in zip(exp, got))
            max_err = max(max_err, err)
            print(f"  导出比对 {role:5s}: 最大系数差 {err:.3g}")
        assert max_err < 1e-7, max_err
        print(f"导出系数与手算一致（最大差 {max_err:.2e}）")
    else:
        print(f"未找到 {csv_path}，跳过导出比对（先运行 make demo）")

    # 三段链路总幅频（复域求和）与各段增益后的期望幅度（输入分量幅度 0.25）
    glo, gmid, ghi = GAINS
    print("\n频点  低段|L|   中段|H*L| 高段|H*H| 总幅频(复域) 期望振幅  dB变化")
    for f in FREQS:
        llo = mag(hand["lp_lo"], f, SR)
        hlo = mag(hand["hp_lo"], f, SR)
        lhi = mag(hand["lp_hi"], f, SR)
        hhi = mag(hand["hp_hi"], f, SR)
        total = total_response(hand, f, SR, GAINS)
        print(f"{f:7.0f} {llo:.5f}  {hlo*lhi:.5f}   {hlo*hhi:.5f}   "
              f"{abs(total):.5f}        {0.25*abs(total):.5f}  {20*math.log10(abs(total)):+.3f}")
    print("\nverify OK")


if __name__ == "__main__":
    main()
