# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## mixer 峰值保护

多轨叠加冲过满量程时，`mx_write_wav` 的 int16 硬削波会产生连续 ±32767 平台（爆点）。`mx_limit` 提供每通道前瞻式砖墙限制：

- 两遍算法：前向按前瞻窗（`attack_ms`）内局部峰值求所需增益，后向按释放时间常数（`release_ms`）平滑，增益只在需要时下降、按速率恢复；
- 内部天花板比阈值低 2e-4（大于一个 int16 量化步长 1/32767），保证浮点不越界且 WAV 量化后不出现平台；
- `enabled=0` 为旁路，缓冲逐样本位级不变（有测试覆盖）；
- 参数校验：`threshold` 必须在 (0,1]，`sample_rate/attack_ms/release_ms` 为正，违例返回 `MX_ERR_PARAM`；轨源通道与总线不符返回 `MX_ERR_CHANNELS`，均可用 `mx_err_str` 取参数提示；
- 报告 `MxLimitReport`：保护前后峰值、平台段数与样本数、被衰减样本数。

`make -C mixer bench` 跑 16/64 轨两组规模，并由 `ref_limit.py` 纯 Python 逐样本手算交叉核对（float32 语义，逐位一致）。
