# gfx-lab

若干小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo.pgm | 线段遮挡的阴影图 |
| asset | C11 | demo.gpak | 资源包：首屏分块压缩与块表 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。
`asset` 另带基准：`make -C asset bench`（100 / 1000 条目，首屏加载 vs 整包解压）。
