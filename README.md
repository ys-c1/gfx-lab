# gfx-lab

六个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| dcomp | C11 | demo.txt + gain_curve_*.csv | 三段式动态压缩（门限分档/比率压缩/限幅兜底） |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## dcomp 动态压缩

三段串联，逐样本处理浮点波形：

1. **门限分档**：`|x| ≤ threshold` 低档（增益 1）；`threshold < |x| < ceiling` 压缩档；`|x| ≥ ceiling` 限幅档。
2. **比率压缩**：压缩档按静态特性 `y = threshold + (|x|-threshold)/ratio` 求增益，经一阶极点包络平滑（`attack_ms`/`release_ms`，系数 `1-exp(-1/(τ·fs))`）。
3. **限幅兜底**：包络启动期来不及下压时硬钳到 `ceiling`，峰值零超调；实际生效增益如实写回曲线。

`bypass=1` 或全程低于门限时输出与输入逐样本一致。非法参数返回 -1 并在 stderr 指出字段名（门限必须 >0、比率必须 ≥1 等）。统计量含三档计数、最大/平均压缩量(dB)、峰值误差、超门限样本数与时长、实测恢复时间（末超门限样本到增益回到 -0.1dB 内；素材内未恢复则置截断标志）。增益曲线可通过 `dc_export_curve` 导出 CSV。
