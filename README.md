# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音、增益与峰值保持电平表 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## mixer 峰值保持电平表

分三段：

1. `mx_meter_peaks()`：每 `block_frames` 帧取一次逐声道绝对峰值，最后一块可以不足一块；
2. `mx_meter_hold()`：块峰值按帧展开并保持。块峰值不低于当前保持电平时抬到该值，
   从块首帧起顶住 `hold_frames` 帧；
3. `mx_meter_release()`：保持到点后每帧减去 `release`（线性电平/帧，下限 0）。
   衰减途中若当前块峰值更高，表针抬到该峰值并重新顶住 `hold_frames` 帧，
   因此表针不会落到当前信号之下，安静后出现的较低新峰也有完整保持。

- `enabled = 0`：只做第 1 段，块峰值逐帧展开，输出与加入保持之前一致；
- `hold_frames = 0`（或负数）返回 `MX_METER_EHOLD`、`release < 0`（或非有限）返回
  `MX_METER_ERELEASE`，`mx_meter_field(rc)` 给出字段名 `hold_frames` / `release`；
  非法配置不写输出；
- 报告：峰值误差 `peak_err`、实测保持帧数 `hold_observed`、越界块/帧/段数
  （块峰值 > 1.0）、最后越界后的恢复帧数（各声道输出 < 1e-3 所需帧数，未恢复为 -1）。
  越界只统计，不削顶。

基准：`make -C mixer bench`（手算包络逐帧对照、2/6 声道误差与耗时、保持×回落对照表、越界与恢复）。
