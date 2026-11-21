#!/usr/bin/env python3
"""Generate deterministic box sets for grid partition benchmarks.

Two scales: small (500 boxes) and large (5000 boxes). Boxes are laid out
on a jittered lattice so adjacent objects frequently overlap across cell
borders, which reproduces the split-across-two-cells duplicate-draw case.

Usage: python3 gen_data.py <out_dir>
Writes small.txt and large.txt: one "x y w h layer" box per line.
"""
import os
import random
import sys


def gen(path: str, n: int, grid_cols: int, cell: float, seed: int) -> None:
    rng = random.Random(seed)
    boxes = []
    per_row = grid_cols
    for i in range(n):
        r, c = divmod(i, per_row)
        # box slightly larger than half a cell -> neighbors always overlap
        size = cell * rng.uniform(0.55, 0.95)
        jitter = cell * 0.15
        x = c * cell + rng.uniform(-jitter, jitter)
        y = r * cell + rng.uniform(-jitter, jitter)
        boxes.append((x, y, size, size, i % 3))
    with open(path, "w") as f:
        f.write(f"# n={n} cell={cell} seed={seed}\n")
        for x, y, w, h, layer in boxes:
            f.write(f"{x:.4f} {y:.4f} {w:.4f} {h:.4f} {layer}\n")


def main(out_dir: str) -> None:
    os.makedirs(out_dir, exist_ok=True)
    gen(os.path.join(out_dir, "small.txt"), n=500, grid_cols=25,
        cell=20.0, seed=20261122)
    gen(os.path.join(out_dir, "large.txt"), n=5000, grid_cols=80,
        cell=20.0, seed=20261123)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
