# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| tile | C11 | demo_*_overlay.ppm / demo_*_outline.csv | 分块轮廓调试：按块边界描轮廓、再叠加到预览 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## tile 模块说明

调试图块边界时分两阶段，互不耦合：

1. `tile_outline_build()` —— 按 `tile_w/tile_h` 划分网格，只生成轮廓表
   （每像素的块号、是否轮廓、每块轮廓像素数），不接触预览像素。
2. `tile_overlay_apply()` —— 把轮廓表按颜色叠加到预览缓冲生成调试图；
   叠加前整图备份，`tile_overlay_restore()` 逐字节还原，关掉调试即回到当前输出。

要点：

- 负参数（`w/h/tile_w/tile_h` 任一项为负、为 0 或超出画布）返回 `TILE_E_ARG`，
  错误信息明确指出是哪一项。
- demo 在 128 与 1024 两档下对比新路径（两阶段）与旧路径 `tile_grid_legacy()`
  （单遍直接画网格），输出轮廓像素数、逐块块号、耗时、叠加误差，并做逐像素对照。
- 轮廓表可导出 CSV（`tile_outline_export_csv`，`x,y,block_id,edge`）。

