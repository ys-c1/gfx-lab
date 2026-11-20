# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音、增益与多通道延迟对齐 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## mixer 多通道延迟对齐

分两段：`mx_align()` 按通道延迟补零对齐样本（通道 c 补 `max_delay - delays[c]`
个样本），`mx_add_aligned()` 把对齐后的块按增益汇总进总线；`mx_add_delayed()`
是两段的合并封装。

- 延迟上限：单通道 `0 <= delays[c] <= MX_MAX_DELAY`（4096 样本，48 kHz 下
  85.33 ms），且对齐后 `frames + max_delay` 必须放得进总线剩余容量。
- 补偿表：`MxAlignTable` 由调用方传入指针，取 `delay[]`、`comp[]`、
  `max_delay`、`compensated`（补入样本总数）。
- 全零延迟时输出与 `mx_add()` 逐字节一致。
- 越界返回负值，`mx_error_field(rc)` 给出字段名：延迟问题为 `delay`，
  通道数为零（或超出补偿表容量）为 `channels`。
