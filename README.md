# gfx-lab

七个独立可构建的小模块，面向 2D 渲染与音频，每个模块自带测试、演示与测量产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化、z 缓冲、alpha 混合（两段式） |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo.pgm | 2D 阴影遮挡与光衰减 |
| asset | C11 | demo.gpak | 打包索引与解包 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## raster：alpha 混合

`rt_set_blend(&t, 1)` 打开混合后，`rt_triangle` 分两段执行：

1. 覆盖判定 + 深度测试，对通过者按 alpha 把源色与目标色混合：
   `out = round(src * a + dst * (1 - a))`
2. 把混合结果写回颜色缓冲与深度缓冲。

`RtStats` 报告 `written`（通过深度测试的像素数）与 `blended`（实际发生颜色混合的像素数）。
`alpha` 越界（含 NaN）返回 `RT_ERR_ALPHA`，目标缓冲为空返回 `RT_ERR_TARGET`，
两者都会在 `t->err_tri` 中给出三角形号。关闭混合时逐像素与旧版行为完全一致。

`make -C raster bench` 运行 32×32 与 256×256 的对比：颜色误差、混合像素数、耗时，
并对若干像素打印逐像素手算过程。
