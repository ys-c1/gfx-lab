#!/usr/bin/env python3
"""独立参考实现与逐对象复核。

运行：python3 -I verify_cull.py [artifacts_dir]
不 import 任何 C 侧代码/同目录模块，只用标准库重新实现：
  1) 朴素（不扩展）剔除
  2) 按运动方向扩盒（ex/ey 与 C 侧公式独立推导）
  3) 扫掠真值 T（ratio=1.0），并对 0.5/1.0/1.5 三档比例统计
     漏剔 = T 中可见却被方案剔除；误剔 = 方案保留但不在 T 中
然后与 C 落盘的保留集合逐对象比对，任何不一致都判失败。
"""
import math
import os
import sys


def overlaps(box, view):
    x, y, w, h = box
    vx, vy, vw, vh = view
    return not (x + w < vx or x > vx + vw or y + h < vy or y > vy + vh)


def expand(box, ratio):
    """返回 (扩展盒, ex, ey)；ex/ey 为单侧扩展像素。"""
    x, y, w, h, _layer, speed, dx, dy = box
    length = math.hypot(dx, dy)
    ex = ratio * speed * abs(dx) / length
    ey = ratio * speed * abs(dy) / length
    return (x - ex, y - ey, w + 2 * ex, h + 2 * ey), ex, ey


def kept_set(boxes, view, ratio):
    """ratio<=0 走朴素；否则扩盒后剔除。返回 (保留下标集合, ex列表, ey列表)。"""
    kept, exs, eys = set(), [], []
    for i, b in enumerate(boxes):
        if ratio <= 0:
            eb, ex, ey = (b[0], b[1], b[2], b[3]), 0.0, 0.0
        else:
            eb, ex, ey = expand(b, ratio)
        exs.append(ex)
        eys.append(ey)
        if overlaps(eb, view):
            kept.add(i)
    return kept, exs, eys


def read_scene(path):
    view, boxes = None, []
    with open(path) as f:
        for line in f:
            p = line.split()
            if p[0] == "view":
                view = tuple(map(float, p[1:5]))
            elif p[0] == "box":
                idx = int(p[1])
                vals = tuple(map(float, p[2:]))
                # x,y,w,h,layer,speed,dx,dy
                boxes.append((idx, vals))
    boxes.sort()
    assert [i for i, _ in boxes] == list(range(len(boxes))), "下标不连续"
    return view, [v for _, v in boxes]


def read_results(path):
    """bench 结果格式：
       naive N ids...        -> p[1]=N, p[2:]=ids
       ratio R N ids...      -> p[1]=R, p[2]=N, p[3:]=ids"""
    out = {}
    with open(path) as f:
        for line in f:
            p = line.split()
            if p[0] == "naive":
                count = int(p[1]); ids = set(map(int, p[2:]))
                assert len(ids) == count, f"{path}: 计数与id数不符 (naive)"
                out["naive"] = ids
            elif p[0] == "ratio":
                key = f"ratio={float(p[1]):.2f}"
                count = int(p[2]); ids = set(map(int, p[3:]))
                assert len(ids) == count, f"{path}: 计数与id数不符 ({key})"
                out[key] = ids
    return out


def read_demo_result(path):
    """demo 结果格式：ratio R / expand i ex ey / kept N ids..."""
    ratio, kept, demo_expand = None, set(), []
    with open(path) as f:
        for line in f:
            p = line.split()
            if p[0] == "ratio":
                ratio = float(p[1])
            elif p[0] == "expand":
                demo_expand.append((int(p[1]), float(p[2]), float(p[3])))
            elif p[0] == "kept":
                count = int(p[1])
                kept = set(map(int, p[2:]))
                assert len(kept) == count
    demo_expand.sort()
    assert [i for i, _, _ in demo_expand] == list(range(len(demo_expand)))
    return ratio, kept, [(ex, ey) for _, ex, ey in demo_expand]


def check(name, boxes, view, c_results=None, demo_expand=None):
    n = len(boxes)
    naive, _, _ = kept_set(boxes, view, 0.0)
    truth, ex1, ey1 = kept_set(boxes, view, 1.0)
    print(f"--- {name}: n={n}, 视口{view}")
    print(f"    朴素保留 {len(naive)}；扫掠真值 T 保留 {len(truth)}")

    # 逐对象：朴素相对真值的漏剔/误剔
    miss = sorted(truth - naive)
    false = sorted(naive - truth)
    print(f"    朴素: 漏剔 {len(miss)} {miss[:8]}{'...' if len(miss)>8 else ''} "
          f"误剔 {len(false)} {false[:8]}")

    ok = True
    for ratio in (0.5, 1.0, 1.5):
        kept, exs, eys = kept_set(boxes, view, ratio)
        m = sorted(truth - kept)
        fc = sorted(kept - truth)
        print(f"    ratio={ratio:.1f}: 保留 {len(kept):5d}  漏剔 {len(m):5d}  误剔 {len(fc):5d}")
        if c_results is not None:
            key = f"ratio={ratio:.2f}"
            if c_results.get(key) != kept:
                ok = False
                print(f"    !! C 结果与参考不符: {key} "
                      f"差集 C-参考={sorted(c_results[key]-kept)[:5]} "
                      f"参考-C={sorted(kept-c_results[key])[:5]}")
        if ratio == 1.0 and demo_expand is not None:
            # 逐对象扩展像素核对
            for i, (cx, cy) in enumerate(demo_expand):
                if abs(cx - exs[i]) > 1e-9 or abs(cy - eys[i]) > 1e-9:
                    ok = False
                    print(f"    !! 扩展像素不符 idx={i} C=({cx},{cy}) ref=({exs[i]},{eys[i]})")

    if c_results is not None and c_results.get("naive") != naive:
        ok = False
        print("    !! C 朴素结果与参考不符")
    # 漏剔为零性质：ratio=1.0 的保留集合必须覆盖整个扫掠真值 T
    kept1, _, _ = kept_set(boxes, view, 1.0)
    if not truth.issubset(kept1):
        ok = False
        print("    !! ratio=1.0 漏剔不为零")
    else:
        print("    ratio=1.0 漏剔为零: 是")
    print("    结果:", "PASS" if ok else "FAIL")
    return ok


def verify_demo(path_scene, path_result):
    view, boxes = read_scene(path_scene)
    ratio, c_kept, demo_expand = read_demo_result(path_result)
    print("\n================ 手推场景逐对象（7 个）================")
    ok = True
    for i, b in enumerate(boxes):
        eb, ex, ey = expand(b, 1.0)
        t_keep = overlaps(eb, view)
        n_keep = overlaps((b[0], b[1], b[2], b[3]), view)
        cx, cy = demo_expand[i]
        if abs(cx - ex) > 1e-6 or abs(cy - ey) > 1e-6:
            ok = False
            print(f"  !! idx={i} 扩展像素不符 C=({cx},{cy}) ref=({ex},{ey})")
        print(f"  idx={i} 起点({b[0]:.0f},{b[1]:.0f},{b[2]:.0f},{b[3]:.0f}) "
              f"speed={b[5]:.0f} d=({b[6]:.0f},{b[7]:.0f}) "
              f"ex={ex:7.3f} ey={ey:7.3f} -> 扩展盒("
              f"{eb[0]:6.2f},{eb[1]:6.2f},{eb[2]:6.2f},{eb[3]:6.2f}) "
              f"朴素={'留' if n_keep else '剔'} 扫掠={'留' if t_keep else '剔'}")

    naive, _, _ = kept_set(boxes, view, 0.0)
    ref_kept, _, _ = kept_set(boxes, view, ratio)
    print(f"  参考朴素保留 {len(naive)} 个；参考 ratio={ratio:.1f} 保留 {len(ref_kept)} 个；"
          f"C 保留 {len(c_kept)} 个")
    if c_kept != ref_kept:
        ok = False
        print(f"  !! C 与参考保留集合不符 C-参考={sorted(c_kept-ref_kept)} "
              f"参考-C={sorted(ref_kept-c_kept)}")
    # 漏剔为零的性质：扫掠真值（ratio=1）相对自身不漏
    truth, _, _ = kept_set(boxes, view, 1.0)
    if not truth.issubset(ref_kept):
        ok = False
        print("  !! 漏剔不为零")
    print("  结果:", "PASS" if ok else "FAIL")
    return ok


def main():
    art = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "artifacts")
    art = os.path.abspath(art)
    all_ok = True

    all_ok &= verify_demo(
        os.path.join(art, "cull_scene.txt"),
        os.path.join(art, "cull_result_r1.0.txt"))

    for n in (1000, 10000):
        view, boxes = read_scene(os.path.join(art, f"cull_scene_{n}.txt"))
        c_results = read_results(os.path.join(art, f"cull_result_{n}.txt"))
        all_ok &= check(f"基准 n={n}", boxes, view, c_results)

    print("\n总判定:", "全部 PASS ✅" if all_ok else "存在 FAIL ❌")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
