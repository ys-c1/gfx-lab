# gfx-lab

七个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo.pgm | 阴影图与两段式量化抖动 |
| asset | C11 | demo.gpak | 资源包读写 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

shadow 另有 `make -C shadow bench`：在演示光场上对比 32 码与 256 码两档的
环带数、补偿量、误差与两段耗时，并对手算样例逐码核对。抖动开关关闭时，
`sh_write_pgm_comp` 的输出与原来的 `sh_write_pgm` 逐像素一致。
