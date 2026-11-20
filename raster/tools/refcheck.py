#!/usr/bin/env python3
"""独立参照实现：按 raster.c 同样的边函数/像素中心/float32 深度规则，
纯 Python 手算每个三角形在早退开/关两条路径下的统计，供与 C 端 RStats 对照。
用法: python3 -I refcheck.py 32|256
"""
import ctypes
import math
import struct
import sys

INF = 1e30


def f32(x):
    return ctypes.c_float(x).value


def edge(ax, ay, bx, by, px, py):
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax)


def build_scene(res):
    s = res / 32.0
    defs = [
        (1, 1, 0.8, "big"), (1, 1, 0.9, "big"), (1, 1, 0.5, "big"),
        (2, 26, 0.3, "small"), (2, 26, 0.9, "small"), (1, 1, 0.1, "big"),
    ]
    tris = []
    for bx0, by0, z, kind in defs:
        L = 31 * s if kind == "big" else 7 * s
        bx, by = bx0 * s, by0 * s
        color = {
            0: (200, 40, 40), 1: (40, 200, 40), 2: (40, 40, 200),
            3: (220, 220, 60), 4: (60, 220, 220), 5: (230, 230, 230),
        }[len(tris)]
        tris.append(((bx, by, z), (bx + L, by, z), (bx, by + L, z), kind, color))
    return tris


def iter_pixels(res, tri):
    """生成 (index, z_interp)：通过边测试的像素，顺序与 C 的 y/x 双层循环一致。"""
    a, b, c, _, _ = tri
    xs = [a[0], b[0], c[0]]
    ys = [a[1], b[1], c[1]]
    minx = max(0, math.floor(min(xs)))
    maxx = min(res - 1, math.ceil(max(xs)))
    miny = max(0, math.floor(min(ys)))
    maxy = min(res - 1, math.ceil(max(ys)))
    for y in range(miny, maxy + 1):
        for x in range(minx, maxx + 1):
            px, py = x + 0.5, y + 0.5
            w0 = edge(b[0], b[1], c[0], c[1], px, py)
            w1 = edge(c[0], c[1], a[0], a[1], px, py)
            w2 = edge(a[0], a[1], b[0], b[1], px, py)
            if w0 < 0 or w1 < 0 or w2 < 0:
                continue
            s_ = w0 + w1 + w2
            z = (w0 * a[2] + w1 * b[2] + w2 * c[2]) / s_
            yield y * res + x, z


def run_mode(res, tris, enable_early):
    n = res * res
    zbuf = [f32(INF)] * n
    rgb = bytearray(n * 3)
    stats = dict(total=0, early=0, degenerate=0, probed=0, shaded=0, filled=0)
    per_tri = []
    for idx, tri in enumerate(tris):
        stats["total"] += 1
        a, b, c, kind, color = tri
        area = edge(a[0], a[1], b[0], b[1], c[0], c[1])
        if abs(area) < 1e-12:
            stats["degenerate"] += 1
            per_tri.append((idx, kind, 0, 0, 0, True))
            continue
        px_list = list(iter_pixels(res, tri))
        inside = len(px_list)
        if enable_early:
            stats["probed"] += inside
            passes = [(i, z) for i, z in px_list if f32(z) < zbuf[i]]
            if not passes:
                stats["early"] += 1
                per_tri.append((idx, kind, inside, 0, 0, True))
                continue
        stats["shaded"] += inside
        written = 0
        for i, z in px_list:
            if f32(z) < zbuf[i]:
                zbuf[i] = f32(z)
                rgb[i*3] = color[0]
                rgb[i*3+1] = color[1]
                rgb[i*3+2] = color[2]
                written += 1
        stats["filled"] += written
        per_tri.append((idx, kind, inside, written, written, False))
    zbytes = struct.pack(f"{n}f", *[f32(v) for v in zbuf])
    return stats, per_tri, zbytes, bytes(rgb)


def main(res):
    tris = build_scene(res)
    print(f"== python ref, resolution {res}x{res} ==")
    for mode in (1, 0):
        st, per, _, _ = run_mode(res, tris, mode)
        tag = "ON " if mode else "OFF"
        print(f"  early={tag}: total={st['total']} early-out={st['early']} "
              f"degenerate={st['degenerate']} probed={st['probed']} "
              f"shaded={st['shaded']} filled={st['filled']} "
              f"inside(probed+shaded)={st['probed'] + st['shaded']}")
        if mode:
            for idx, kind, inside, dep, filled, early in per:
                print(f"    T{idx} {kind:5s}: edge-pass={inside:6d} filled={filled:6d} "
                      f"early_out={'yes' if early else 'no'}")
    # 两模式最终缓冲必须一致（画面相同）
    _, _, z_on, rgb_on = run_mode(res, tris, 1)
    _, _, z_off, rgb_off = run_mode(res, tris, 0)
    print(f"  z-buffer identical across modes: {z_on == z_off}")
    print(f"  rgb identical across modes: {rgb_on == rgb_off}")
    return rgb_off


def write_ppm(path, res, rgb):
    with open(path, "wb") as f:
        f.write(f"P6\n{res} {res}\n255\n".encode())
        f.write(rgb)


if __name__ == "__main__":
    res = int(sys.argv[1]) if len(sys.argv) > 1 else 32
    rgb = main(res)
    if "--ppm" in sys.argv:
        out = sys.argv[sys.argv.index("--ppm") + 1]
        write_ppm(out, res, rgb)
        print(f"  wrote {out}")
