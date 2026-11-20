# gfx-lab

七个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo.pgm | 2D 阴影贴图 |
| asset | C11 | demo.gpak | 资源打包与提取 |
| xfade | C11 | demo_*.wav / curve_*.csv | 切歌交叉淡化：权重曲线、逐样本合成、接缝跳变检查 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## xfade 说明

解决硬切歌时的咔哒声，分三段：

1. **权重曲线（与采样率无关）**：在归一化时间 `t∈[0,1]` 上定义，
   `linear`（线性，对照）与 `equal-power`（cos/sin 等功率）两种曲线；
   `XF_NONE` 表示不淡化。`xf_save_curve` / `xf_export_curve` 可把曲线存成 CSV。
2. **逐样本交叉合成**：A 的末尾 n 帧与 B 的开头 n 帧按权重
   `out = wa*A + wb*B` 合成；`XF_NONE` 时 n=0，输出与直接拼接逐样本一致。
3. **接缝跳变检查**：`xf_seam_jump` 在接缝窗口内逐声道取相邻样本最大绝对差。

参数错误返回负错误码，`xf_error_field` 指出出错字段：
时长为零（`fade_ms`）、曲线非法（`curve`）、重叠超长（`fade_ms(overlap)`）等。
演示对 1 秒与 10 秒素材比较硬切/线性/等功率的接缝跳变、
对双精度参考的样本误差与 CPU 耗时，并导出权重曲线与可听 WAV。
