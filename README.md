# gfx-lab

小模块集合，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo.pgm | 阴影贴图 |
| asset | C11 | demo.gpak | 资产打包 |
| jit | C11 | demo_report.txt + demo_jitter_*.csv | 抖动叠样：三段式（抖动偏移 → 取整 → 加权合成）与梳状染色指标 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## jit 模块

同一样本按固定周期反复叠加会在频谱上形成梳齿（梳状染色）。处理严格分三段，各自是独立函数：

1. `jit_offsets` —— 按确定性 xorshift32 抖动序列生成连续采样偏移 `d[k] = k*P + jitter[k]`，`jitter ~ U[-A,A]`；
2. `jit_quantize` —— 连续偏移按半数远离零规则取整，并对整数落点做越界校验；
3. `jit_synthesize` —— 按每路权重（默认 1/N）把素材叠加到整数采样点。`jit_synthesize_frac` 提供线性插值的连续参考，用于量化取整误差。

要点：

- `amp = 0` 时不生成抖动，逐样本等价普通重复叠加（有测试对照朴素实现）。
- `src_len <= 0`、参数为负、偏移越界、输出容量不足均返回错误码，`jit_last_error()` 指出具体参数名。
- `jit_export_csv` 导出完整抖动序列（基准点、连续偏移、连续/取整抖动、整数落点、权重）。
- `jit_comb_metric` 对叠加位置的单位脉冲响应做 radix-2 FFT，给出峰均比（crest dB）、谱峰 bin、半峰以上梳齿数与频谱平坦度；无抖动与抖动方案在 demo 报告中直接对照。
- demo 对 1 秒与 10 秒素材输出量峰值误差、取整后抖动路数、各段耗时与抖动长度。
