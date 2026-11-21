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

## mip 层链：两段式内存预算构建

层链一次性建满会按 4/3 倍源图大小吃内存，因此拆成两段：

1. `mip_plan(w, h, max_bytes, plan, err_idx)` 只做算术、不分配：算出理论层数
   `full_levels`、预算允许的层数上限 `cap_levels` 与逐层字节数。
2. `mip_build(c, src, w, h, max_bytes)` 先调 `mip_plan` 得上限，再从第 0 层起
   逐层 `malloc`、逐层 2x2 盒式降采样；任一层分配失败立即停止，已生成的层保持
   可用，由 `mip_free` 回收。

- `max_bytes == 0` 表示无预算，构建到 1x1 的完整链，结果与旧版本逐字节一致。
- **降级策略**：从第 0 层起累加每层像素字节，第一个使累计值超过 `max_bytes` 的层
  不再生成（即保留层号 `0 .. cap_levels-1`）；预算连第 0 层都不够时仍保留第 0 层。
  降级时 `MipChain.degraded=1`，`cap_levels` 即降级后的层数。
- 错误码：`0` 成功，`-1` 空指针/分配失败，`-2` 非法输入；`err_idx` 标出第一个非法
  参数的索引（c=0, src=1, w=2, h=3, max_bytes=4, plan=5），成功时为 -1。
- 基准与逐层核对：`make -C mip bench`，对 1 千（32x32）与 1 万（100x100）规模报告
  误差、耗时（200 轮平均）、生成层数、进程内存峰值（VmHWM）与每个层号的尺寸/字节。

