#!/usr/bin/env python3
"""独立复核 C 侧软削波产物: 解析解重算、CSV/报告/WAV 逐项对照、SVG 出图。
仅使用标准库。用法: python3 -I verify.py <artifacts-dir>"""
import csv
import math
import os
import struct
import sys

SR = 48000
FRAMES = 4800
KNEE = 0.8
CEIL = 1.0
LEVELS = (0.6, 2.0)
CHANS = (2, 8)


def analytic(knee, ceil_, x):
    ax = abs(x)
    if ax <= knee:
        y = ax
    elif ceil_ <= knee:          # C == T: 硬切退化
        y = knee
    else:
        d = ceil_ - knee
        y = ceil_ - d * math.exp(-(ax - knee) / d)
    return -y if x < 0 else y


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    ch = struct.unpack_from("<H", data, 22)[0]
    nbytes = struct.unpack_from("<I", data, 40)[0]
    pcm = data[44:44 + nbytes]
    samples = list(struct.unpack("<%dh" % (nbytes // 2), pcm))
    return ch, [s / 32767.0 for s in samples]


def check_curve_csv(d):
    path = os.path.join(d, "curve.csv")
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    assert len(rows) == 2048, len(rows)
    max_a, max_lut, max_ident = 0.0, 0.0, 0.0
    knee_rows = 0
    prev_x, mono = None, True
    for r in rows:
        x, ya, yl = float(r["input"]), float(r["analytic"]), float(r["lut"])
        ea = abs(ya - analytic(KNEE, CEIL, x))
        el = abs(yl - analytic(KNEE, CEIL, x))
        max_a, max_lut = max(max_a, ea), max(max_lut, el)
        if abs(x) < KNEE:
            max_ident = max(max_ident, abs(yl - x))
        if abs(abs(x) - KNEE) < 0.01:
            knee_rows += 1
        if prev_x is not None and yl < prev_x - 1e-9:
            mono = False
        prev_x = yl
    # 拐点处独立复核: f(T)=T 且左右导数均为 1 (C1 连续)
    h = 1e-5
    dl = (analytic(KNEE, CEIL, KNEE) - analytic(KNEE, CEIL, KNEE - h)) / h
    dr = (analytic(KNEE, CEIL, KNEE + h) - analytic(KNEE, CEIL, KNEE)) / h
    print(f"[csv] rows=2048 analytic_reimpl_maxerr={max_a:.3e} "
          f"lut_vs_analytic_maxerr={max_lut:.3e} below_knee_identity_maxerr={max_ident:.3e} "
          f"monotonic={mono} knee_nearby_rows={knee_rows} "
          f"f(T)={analytic(KNEE, CEIL, KNEE):.9f} f'(T-)={dl:.6f} f'(T+)={dr:.6f}")
    assert max_a < 1e-6 and max_lut < 2e-3 and max_ident == 0.0 and mono
    assert abs(analytic(KNEE, CEIL, KNEE) - KNEE) < 1e-9
    assert abs(dl - 1.0) < 1e-3 and abs(dr - 1.0) < 1e-3
    return rows


def check_round(d, level, ch):
    tag = f"lvl{level}_ch{ch}"
    ch_on, on = read_wav(os.path.join(d, f"round_{tag}_on.wav"))
    ch_off, off = read_wav(os.path.join(d, f"round_{tag}_off.wav"))
    assert ch_on == ch and ch_off == ch

    n = FRAMES * ch
    assert len(on) == n and len(off) == n
    sig = [level * math.sin(2 * math.pi * 440 * (i // ch) / SR)
           for i in range(n)]

    # 关闭路径: 440 正弦峰值等于 level(超过 ±1 的部分被 wav 写入端硬切)
    peak_in = max(abs(s) for s in sig)
    # 开启路径: 逐样本对解析解
    err, peak_out, qerr = 0.0, 0.0, 0.0
    clip_samples, segments, first_frame = 0, 0, -1
    for c in range(ch):
        in_seg = False
        for i in range(FRAMES):
            idx = i * ch + c
            x = sig[idx]
            ya = analytic(KNEE, CEIL, x)
            # C 端先软削波再写 16-bit; 这里把解析解做同样的 16-bit 量化
            q = max(-32768, min(32767, round(ya * 32767)))
            exp = q / 32767.0
            err = max(err, abs(on[idx] - exp))
            qerr = max(qerr, abs(exp - ya))
            peak_out = max(peak_out, abs(on[idx]))
            over = abs(x) >= KNEE
            if over:
                clip_samples += 1
                if first_frame < 0 or i < first_frame:
                    first_frame = i
            if over and not in_seg:
                segments += 1
                in_seg = True
            elif not over:
                in_seg = False

    # 与 report.txt 中的 C 侧统计核对
    rep_line = next(l for l in open(os.path.join(d, "report.txt"))
                    if l.startswith(f"level={level} ch={ch} "))
    kv = dict(tok.split("=", 1) for tok in rep_line.split())
    assert abs(float(kv["peak_in"]) - peak_in) < 1e-6
    # report 记浮点峰值, 这里是 WAV 16-bit 量化后峰值, 差一个量化步长以内
    assert abs(float(kv["peak_out"]) - peak_out) <= 1.0 / 32767.0 + 1e-9
    assert int(kv["clip_samples"]) == clip_samples
    assert int(kv["segments"]) == segments
    assert int(kv["first_frame"]) == first_frame

    # 正弦周期 1/440 s = 109.09 帧; 每周期 2 个越界游程, 100ms 内 44 个周期
    print(f"[round {tag}] peak_in={peak_in:.6f} peak_out={peak_out:.6f} "
          f"first_frame={first_frame} first_ms={1000*first_frame/SR:.3f} "
          f"segments={segments} clip_samples={clip_samples} "
          f"wav_vs_analytic_maxerr={err:.3e} q16_maxerr={qerr:.3e}")
    assert peak_out <= CEIL + 1e-9
    if level < KNEE:
        assert segments == 0 and clip_samples == 0
        assert abs(peak_out - peak_in) < 2e-4
    else:
        assert segments == 88 * ch       # 每声道每周期 2 段, 44 周期
        assert peak_out < CEIL
        assert err < 2e-3
    return dict(level=level, ch=ch, peak_in=peak_in, peak_out=peak_out,
                first_frame=first_frame, segments=segments,
                clip_samples=clip_samples, err=err)


def write_svg(rows, d):
    W, H, ml, mr, mt, mb = 760, 520, 64, 24, 36, 48
    pw, ph = W - ml - mr, H - mt - mb
    xmin, xmax, ymin, ymax = -4.0, 4.0, -1.15, 1.15

    def X(x): return ml + (x - xmin) / (xmax - xmin) * pw
    def Y(y): return mt + (ymax - y) / (ymax - ymin) * ph

    def path(points):
        return " ".join(("M" if i == 0 else "L") +
                        f"{X(x):.2f},{Y(y):.2f}" for i, (x, y) in enumerate(points))

    ident = [(x, x) for x in [xmin + (xmax - xmin) * i / 200 for i in range(201)]]
    analytic_pts = [(float(r["input"]), float(r["analytic"])) for r in rows]
    lut_pts = [(float(r["input"]), float(r["lut"])) for r in rows]
    hard = [(x, max(-1.0, min(1.0, x))) for x, _ in ident]

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
           f'viewBox="0 0 {W} {H}" font-family="sans-serif" font-size="12">',
           '<rect width="100%" height="100%" fill="white"/>',
           f'<text x="{W/2}" y="22" text-anchor="middle" font-size="16" '
           'font-weight="bold">mixer 软削波传递曲线 (knee T=0.8, ceiling C=1.0)</text>']

    for g in [i / 4 for i in range(-16, 17, 4)]:
        out.append(f'<line x1="{X(xmin):.1f}" y1="{Y(g):.1f}" x2="{X(xmax):.1f}" '
                   f'y2="{Y(g):.1f}" stroke="#eee"/>')
        out.append(f'<text x="{ml-8}" y="{Y(g)+4:.1f}" text-anchor="end" fill="#666">{g:g}</text>')
    for gv in range(-4, 5):
        out.append(f'<line x1="{X(gv):.1f}" y1="{Y(ymin):.1f}" x2="{X(gv):.1f}" '
                   f'y2="{Y(ymax):.1f}" stroke="#f2f2f2"/>')
        out.append(f'<text x="{X(gv):.1f}" y="{H-mb+18}" text-anchor="middle" fill="#666">{gv}</text>')

    out.append(f'<line x1="{ml}" y1="{Y(0):.1f}" x2="{W-mr}" y2="{Y(0):.1f}" stroke="#999"/>')
    out.append(f'<line x1="{X(0):.1f}" y1="{mt}" x2="{X(0):.1f}" y2="{H-mb}" stroke="#999"/>')

    # 拐点与 ceiling 标注
    for t in (-KNEE, KNEE):
        out.append(f'<line x1="{X(t):.1f}" y1="{mt}" x2="{X(t):.1f}" y2="{H-mb}" '
                   'stroke="#d9a441" stroke-dasharray="4 3"/>')
        out.append(f'<text x="{X(t):.1f}" y="{mt-6}" text-anchor="middle" fill="#a9741a">knee {t:g}</text>')
    for c in (-CEIL, CEIL):
        out.append(f'<line x1="{ml}" y1="{Y(c):.1f}" x2="{W-mr}" y2="{Y(c):.1f}" '
                   'stroke="#c0504d" stroke-dasharray="2 3"/>')
        out.append(f'<text x="{W-mr-4}" y="{Y(c)-3:.1f}" text-anchor="end" fill="#a33">'
                   f'ceiling {c:g}</text>')

    out.append(f'<path d="{path(hard)}" fill="none" stroke="#c0504d" '
               'stroke-width="1.4" stroke-dasharray="6 4"/>')
    out.append(f'<path d="{path(ident)}" fill="none" stroke="#999" stroke-width="1"/>')
    out.append(f'<path d="{path(analytic_pts)}" fill="none" stroke="#1f6fb2" stroke-width="2.2"/>')
    out.append(f'<path d="{path(lut_pts)}" fill="none" stroke="#2e8b57" stroke-width="1" '
               'stroke-dasharray="2 2" opacity="0.85"/>')

    lx, ly = ml + 14, mt + 14
    legend = [("#1f6fb2", "软削波解析解 f(x)"),
              ("#2e8b57", "LUT 插值输出 (与解析解重合)"),
              ("#999", "恒等线 y=x (拐点以下)"),
              ("#c0504d", "硬削波 (旧行为, 对比)")]
    for i, (col, label) in enumerate(legend):
        y = ly + i * 20
        out.append(f'<line x1="{lx}" y1="{y}" x2="{lx+26}" y2="{y}" stroke="{col}" stroke-width="2.4"/>')
        out.append(f'<text x="{lx+32}" y="{y+4}">{label}</text>')

    out.append(f'<text x="{W/2}" y="{H-10}" text-anchor="middle" fill="#444">输入幅度 x</text>')
    out.append(f'<text x="18" y="{H/2}" text-anchor="middle" fill="#444" '
               'transform="rotate(-90 18 %d)">输出幅度 y</text>' % (H // 2))
    out.append("</svg>")
    with open(os.path.join(d, "curve.svg"), "w") as f:
        f.write("\n".join(out))
    print(f"[svg] -> {os.path.join(d, 'curve.svg')}")


def main():
    d = sys.argv[1]
    rows = check_curve_csv(d)
    summaries = []
    for level in LEVELS:
        for ch in CHANS:
            summaries.append(check_round(d, level, ch))
    write_svg(rows, d)
    print("\n全部对照通过: %d 轮, 曲线表 %d 行" % (len(summaries), len(rows)))


if __name__ == "__main__":
    main()
