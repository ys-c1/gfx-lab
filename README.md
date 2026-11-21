# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| reverb | C11 | demo.wav | 混响尾巴：按衰减定时长、补零、逐样本叠加 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## reverb 混响尾巴：三段式

反馈梳状网络的尾巴原先会被输入块的长度直接截断，听感生硬。现在拆成三段：

1. `rv_tail_len(rate, decay_sec, &tail)` 只做算术：尾巴长度 = `ceil(decay_sec * rate)`
   帧，即 RT60；每个抽头的反馈系数取 `fb = 10^(-3*delay/(decay_sec*rate))`，
   走一趟延迟线正好衰减 60 dB，所以这段时长足以让网络响完。
2. `rv_pad(in, frames, channels, tail, padded)` 把输入拷到前 `frames` 帧，后面补
   `tail` 帧零；`tail == 0` 时就是逐样本拷贝。
3. `rv_sum(r, padded, padded_frames, out)` 逐样本算
   `out = dry*padded + wet*Σ gain_t * 延迟线读数`，同时把输入与反馈写回延迟线。

- 一次调用：`rv_process()` 依次走完三段，返回输出帧数（`frames + tail`）。
- 退化路径：`rv_process_legacy()` 即 `tail = 0`，只渲染输入、丢掉尾巴，输出与旧版本
  **逐字节一致**；因为网络是因果的，完整三段输出的前 `frames` 帧也与它逐字节一致。
  `dry=1, wet=0` 时输出是输入的逐字节拷贝（延迟线仍推进，便于后续开湿声）。
- 空输入：`frames <= 0` 一律返回 `RV_ERR_EMPTY`，`rv_strerror()` 说明「input is
  empty (frames <= 0)」；其余错误码分别指向 `rate`、`decay_sec`、`channels`、`delay`、
  `gain`、`tail`、缓冲区重叠等具体字段。
- `rv_export_curve()` 把湿声脉冲响应按帧导出为 CSV：
  `frame,hand,measured,abs_err,level_db_hand,level_db_measured,taps`。`hand` 是双精度
  闭式值 `wet * Σ gain_t * fb_t^(n/delay_t - 1)`（仅当 `delay_t | n`），逐行与实测值
  比较，`RvCurveStats` 回报行数、超差行数与最大绝对/相对误差，可与手算表一一核对。

基准与逐项核对：`make -C reverb demo`，对 512 帧（约 10.7 ms，短于尾巴）与 48 万帧
（10 s，远长于尾巴）两种规模报告尾巴长度、截断样本数、峰值、与手算表的误差、逐块耗时、
三段与一次调用/退化路径的逐字节一致性，并写出 `tail_curve_small.csv`、
`tail_curve_large.csv` 与 `demo.wav`。
