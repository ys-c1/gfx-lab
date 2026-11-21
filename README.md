# gfx-lab

面向 2D 渲染与音频的小模块，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo.pgm | 阴影图 |
| asset | C11 | demo.gpak | GPAK 资产包读写 |
| reindex | C11 | mapping_*.csv, refs_old/new_*.txt | 槽位重排后的两阶段引用修复 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## reindex：索引轮换后的引用修复

索引轮换一轮后，外部保存的旧索引会指向错误的图片。修复严格分两阶段：

1. `rx_build` 按 旧索引 → 新索引 建映射表（置换入参，重复认领同一新槽位计入 `conflicts`，先认领者生效）；
2. `rx_rewrite` 按表批量就地改写引用，命中冲突/越界的引用保留原值并计冲突。

`rx_apply` 带开关：`enabled=0` 完全不碰引用，输出与旧路径逐字节一致（关掉必须回到当前输出）；
`enabled=1` 才执行建表+改写。任何负数参数硬报错，`RxErr` 指出参数名与第几项。
映射表可经 `rx_export` 导出 CSV。`demo` 在 128/1024 两档对比像素数、耗时、改写条数、冲突数、
映射长度，并导出旧/新两份引用文件与指纹做对照。
