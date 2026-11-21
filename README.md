# gfx-lab

面向 2D 渲染与音频的小模块，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo_hard.pgm / demo_soft.pgm | 硬边阴影与 PCF 多点采样软化 |
| asset | C11 | demo.gpak | 资源打包与索引读写 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## shadow 软化（PCF）

核内 S×S 多点采样，按未遮挡覆盖比例平均后与中心光照合成；`pcf == NULL`
时与硬边渲染逐位一致。`pcf.samples<=0`、`pcf.kernel` 非正奇数分别返回
`-3`/`-4` 并由 `sh_errstr` 指出字段。核表可经 `sh_kernel_build` /
`sh_kernel_write` 导出；`make -C shadow bench` 输出 128/512 两档的边缘误差、
采样次数、耗时与软化宽度（`bench_report.csv`、`kernel_s4_k5.txt`、
`kernel_width_table.txt`）。核表与软化宽度对照见 [shadow/KERNEL.md](shadow/KERNEL.md)。
