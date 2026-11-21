# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| tile | C11 | demo.ppm、colors_1x.csv、colors_2x.csv | 按块读边框色并按色填充图块边缘（含老路径兼容、越界检测） |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## tile 模块说明

解决“整图切块后，所有块只能用同一种边框色、拼图接缝露底”的问题，分两段：

1. `tile_read_colors`：按块对四条边采样，得到每块独立的边框色与采样次数；
2. `tile_fill_edges`：按每个块自己的边框色填充其外缘。
   - `enable_new=0` 时走老路径（统一 `legacy_rgb`），遍历顺序与写法固定，输出字节与旧逻辑一致；
   - `enable_new!=0` 时逐块取 `colors[]` 的色。

要点：

- 两档倍率（1x / 2x）：2x 在相邻采样点间插入中点，每边采样数 `2*len-1`。
- 统计：采样次数、填充像素、统计误差（各块色相对全局均值的 MAE）、边框误差（边缘像素相对本块均值的 MAE）。
- 越界：`tile_check_bounds` 与填充前逐块检查，检出即返回 `TILE_ERANGE` 并在 `oob_block` 报出块号（行优先 `row*cols+col`）。
- 色表可导出为 CSV（`tile_export_colors`），用于和纸笔推导数值对照；`test.c` 内含独立参考实现逐字节校验新/老路径。

