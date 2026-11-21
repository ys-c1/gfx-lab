# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| grid | C11 | demo.txt | 网格划分两段合并（重叠插入→并查集合并→唯一归属），去重复绘制 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

grid 模块另带两规模（500 / 5000 盒子）基准与 Python 参考对照：
`bash grid/tools/run_compare.sh`。
