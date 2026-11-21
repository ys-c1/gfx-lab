# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化、z 缓冲与线框叠加 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## raster：线框叠加

线框有独立叠加层，不会被之后画的填充盖住；合成分三段——按边函数描边、
按层号合并、按不透明度混合。关闭时不改变任何字节。细节与 64/512 实测见
[`raster/WIREFRAME.md`](raster/WIREFRAME.md)；基准：`make -C raster bench`。
