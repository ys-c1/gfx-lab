# gfx-lab

小模块集合，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| eq | C11 | demo_eq.wav, demo_coef.csv | 三段（低/中/高）分频均衡 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## eq 三段均衡

一阶互补分频（双线性变换）：`low=LPF(f_lo,x)`、`high=x-low`、`mid=LPF(f_hi,high)`、
`treble=high-mid`，输出 `g_lo·low + g_mid·mid + g_hi·treble`。

- `enabled=0` 时逐样本原样拷贝（位一致）；单位增益启用时完全重构（误差 < 1e-5）
- 边界倒置（`f_lo>=f_hi`、越 Nyquist）与增益越界（`[0,16]`）返回带字段名的错误码
- 每声道统计输入/输出峰值与过载（`|样本|>1`）次数；四节滤波器系数可导出 CSV
- `make -C eq verify` 用独立 Python（纯 stdlib，`-I`）手算系数与复频响应对照
- 演示在 2/6 声道下报告各段增益误差（< 0.005 dB）、峰值变化、过载次数与耗时
