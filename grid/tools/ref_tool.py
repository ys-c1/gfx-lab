#!/usr/bin/env python3
"""Reference implementation of the two-pass grid partition.

Independent of the C module: union-find merge plus cell insertion written
from scratch in pure Python. Emits the same metrics the C demo prints, so
run_compare.py can diff them numerically.

Usage:
    python3 -I ref_tool.py <input-boxes.txt> <cols> <rows>
Output (one line, key=value pairs space-separated):
    n= groups= merge_pairs= dup1= cand1= dup2= cand2= time1= merge_time= time2=
"""
import sys
import time

MAX_OBJECTS = 100_000
MAX_CELLS = 1_000_000


def load(path):
    boxes = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            x, y, w, h = map(float, p[:4])
            layer = int(p[4]) if len(p) > 4 else 0
            if w <= 0 or h <= 0:
                raise ValueError(f"invalid box: {line}")
            boxes.append((x, y, w, h, layer))
    return boxes


def overlaps(a, b):
    return not (a[0] + a[2] < b[0] or a[0] > b[0] + b[2]
                or a[1] + a[3] < b[1] or a[1] > b[1] + b[3])


def insert(boxes, gw, gh, cols, rows):
    """Overlap insertion (pass 1): return (entries, duplicate registrations)."""
    if len(boxes) > MAX_OBJECTS:
        raise ValueError(f"GRID_MAX_OBJECTS={MAX_OBJECTS}")
    if cols * rows > MAX_CELLS:
        raise ValueError(f"GRID_MAX_CELLS={MAX_CELLS}")
    cw, ch = gw / cols, gh / rows
    entries = 0
    seen = set()
    for idx, (x, y, w, h, _layer) in enumerate(boxes):
        x1, y1 = x + w, y + h
        if x1 < 0 or x > gw or y1 < 0 or y > gh:
            continue
        x0c = min(max(int(max(x, 0) / cw), 0), cols - 1)
        x1c = min(max(int(min(x1, gw) / cw), 0), cols - 1)
        y0c = min(max(int(max(y, 0) / ch), 0), rows - 1)
        y1c = min(max(int(min(y1, gh) / ch), 0), rows - 1)
        for cy in range(y0c, y1c + 1):
            for cx in range(x0c, x1c + 1):
                entries += 1
                seen.add(idx)
    return entries, entries - len(seen)


def assign_unique(boxes, gw, gh, cols, rows):
    """Unique owner-cell assignment (pass 2): each box scheduled once."""
    if len(boxes) > MAX_OBJECTS:
        raise ValueError(f"GRID_MAX_OBJECTS={MAX_OBJECTS}")
    if cols * rows > MAX_CELLS:
        raise ValueError(f"GRID_MAX_CELLS={MAX_CELLS}")
    cw, ch = gw / cols, gh / rows
    entries = 0
    for x, y, _w, _h, _layer in boxes:
        if x < 0 or y < 0 or x > gw or y > gh:
            continue
        cx = min(max(int(x / cw), 0), cols - 1)
        cy = min(max(int(y / ch), 0), rows - 1)
        _ = (cx, cy)
        entries += 1
    return entries, 0


def merge(boxes):
    n = len(boxes)
    parent = list(range(n))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            if ra < rb:
                parent[rb] = ra
            else:
                parent[ra] = rb

    pairs = 0
    for i in range(n):
        for j in range(i + 1, n):
            if overlaps(boxes[i], boxes[j]):
                pairs += 1
                union(i, j)

    roots = {}
    group_ids = []
    for i in range(n):
        r = find(i)
        if r not in roots:
            roots[r] = len(roots)
        group_ids.append(roots[r])

    merged = []
    for _ in range(len(roots)):
        merged.append([1e300, 1e300, -1.0, -1.0, 0])
    for i, (x, y, w, h, layer) in enumerate(boxes):
        g = group_ids[i]
        m = merged[g]
        m[0] = min(m[0], x)
        m[1] = min(m[1], y)
        m[2] = max(m[2], x + w)
        m[3] = max(m[3], y + h)
    for m in merged:
        m[2] -= m[0]
        m[3] -= m[1]
    return merged, group_ids, pairs


def main():
    path, cols, rows = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    boxes = load(path)
    n = len(boxes)
    gw = max((b[0] + b[2] for b in boxes), default=1.0)
    gh = max((b[1] + b[3] for b in boxes), default=1.0)

    t0 = time.perf_counter()
    cand1, dup1 = insert(boxes, gw, gh, cols, rows)
    t1 = time.perf_counter()
    merged, group_ids, pairs = merge(boxes)
    t2 = time.perf_counter()
    cand2, dup2 = assign_unique(merged, gw, gh, cols, rows)
    t3 = time.perf_counter()

    print(f"n={n} groups={len(merged)} merge_pairs={pairs} "
          f"dup1={dup1} cand1={cand1} dup2={dup2} cand2={cand2} "
          f"time1={t1 - t0:.6f} merge_time={t2 - t1:.6f} time2={t3 - t2:.6f}")


if __name__ == "__main__":
    main()
