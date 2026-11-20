# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 三段式货架图集打包（高度重排 / 90° 旋转 / 坐标回填） |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

atlas 另有基准：`make -C atlas bench`，在 128/512 两种条带宽度下对比四种配置的占用率、条带数与耗时，并与手工逐块摆放对照、做独立几何校验。`atlas_pack_ex(..., 0, NULL, ...)` 的布局与原始 `atlas_pack` 完全一致。
