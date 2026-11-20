# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo.pgm / demo_soft.pgm | 硬阴影与三段式面积光软阴影 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## shadow 软阴影

三段式管线（`sh_render_ex`）：

1. **半影带宽**：相似三角形解析公式 `W = 2·r·(1−t)/t`（`t` 为光源→遮挡体占光源→接收点的距离比），见 `sh_penumbra_width`；
2. **多点本影**：在面积光圆盘上按黄金角向日葵布点，每个光源点渲染一张硬本影图；
3. **合成**：N 张本影图逐像素平均得到软阴影。

`ShPenumbra.enable = 0`（或传 `NULL`）走原硬阴影路径，输出与 `sh_render` 逐像素一致（测试用 `memcmp` 校验）。
`pen.samples = SH_SAMPLES_ADAPTIVE` 时由第 1 段的最大带宽决定采样数。
逐像素蒙特卡洛基准 `sh_render_ref`；分辨率/带宽误差/采样数/耗时对比：`make -C shadow bench`。

错误均带字段名（`ShError.field`）：光源半径为零 → `light.src_radius`、
采样数为零 → `pen.samples`、遮挡段退化 → `segs[i]`。
