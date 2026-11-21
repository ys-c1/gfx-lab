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

## mixer 软削波 (softclip)

两段式软削波，解决混音超动态范围后 `mx_write_wav` 硬切听感炸裂的问题：

1. **建曲线** `sc_build_curve`：按拐点 T (knee) 与上限 C (ceiling) 预计算查找表。
   曲线为奇对称指数软膝，`x ≤ T` 恒等、`x > T` 时
   `f(x)=C-(C-T)·exp(-(x-T)/(C-T))`，拐点处 `f(T)=T、f'(T)=1`（C1 连续），
   渐近收敛到 C 不硬切；`C==T` 退化为硬切。
2. **逐样本套用** `sc_apply`：拐点以下位级直通，拐点以上 LUT 线性插值，
   表尾之外走解析尾部。未启用（空指针或 `valid==0`）时完全直通。

非法组合（`knee<=0`、`ceiling<knee`、`tabs<4`、NaN/Inf 等）返回 -2，
并通过位掩码一次性列出全部冲突项。

- 单元测试：`make -C mixer test`（含冲突拒绝、C1 连续、奇对称、单调性、解析解对照）
- 演示与指标：`make -C mixer demo`，两档输入 (0.6/2.0) × 2/8 声道各一轮，
  报告量化误差、耗时、削波段数、拐点首帧位置、峰值。
- 独立复核与出图：`make -C mixer verify`（Python 标准库，`-I` 运行），
  逐项对解析解复核并生成 `artifacts/softclip/` 下的 CSV 曲线表与 SVG 图。
- 关闭时 `demo.wav` 与引入本特性前逐字节一致（有回归校验）。
