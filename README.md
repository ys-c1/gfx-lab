# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| asset | C11 | demo.gpak | GPAK 资源包读写，v1→v2 两阶段版本迁移 |
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo.pgm | 阴影图生成 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

asset 模块额外提供：`make -C asset bench`（v1/v2 两档计时与误差对比）、
`make -C asset verify`（用 `tools/hand_parse.py` 手写解析器交叉核对）。
