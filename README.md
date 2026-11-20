# gfx-lab

六个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| zorder | C11 | demo.txt | 莫顿码（Z 序）图块重排与局部性统计 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## zorder 说明

按莫顿码重排二维图块，让空间相邻的图块在一维内存里尽量相邻，改善缩图时的缓存命中。分两阶段：

1. `zo_reorder`：按莫顿码（`ZO_MORTON`）、面积（`ZO_AREA`，对照）或保持原序（`ZO_NONE`）生成 `order`（新位置→原图块序号），并统计全图跳转次数 `jumps`。
2. `zo_backfill`：回填逆索引 `remap`（原图块序号→新位置）。

`ZO_NONE` 不打乱顺序，布局不变、跳转次数为 0。尺寸非 2 的幂返回 `-2`；外部码位越界返回 `-4`、重复返回 `-3`，`err_seq` 指出首个出错序号。`zo_window_stats` 以滑动缩图窗口统计窗口内跳转次数与地址跨度比（越接近 1 越紧凑）。
