# gfx-lab

六个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| halftone | C11 | demo-64.ppm / demo-512.ppm / demo-matrix.txt / demo-matrix.pgm | 两段式半透明网点雾 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## halftone：两段式半透明网点雾

半透明雾不再用实心填充，而是分两段处理：

1. **网点判定**：Bayer 有序抖动矩阵逐像素平铺，像素量化等级对应阈值，矩阵秩低于阈值才写网点；
2. **覆盖率混色**：覆盖率 = 矩阵中低于阈值的格点数 / 总格点数，只对落在网点上的像素按覆盖率在底色与雾色间做整数混色。

- `ht_apply_fast`：cover 查找表 + 每像素一次矩阵索引；`ht_apply_brute`：逐像素遍历整个矩阵的暴力对照，两者结果逐字节一致；
- `enabled=0` 时输出保持原样、不读取 alpha；
- 非法 alpha（越界或 NaN）返回 `-2`，`ht_error()` 给出位置，如 `alpha[7]=1.5 out of range [0,1]`，且输出不被破坏；
- 演示在 64×64 与 512×512 下报告网点覆盖数、混色误差、像素数与两条路径的耗时，并导出网点矩阵（文本 + PGM）。

