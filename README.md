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

## cull 模块：动画视口两段式剔除

视口动画不再每帧对全部 box 重建剔除结果，而是分两段：

1. **插值中间视口**：`cull_view_lerp(v0, v1, t)` 按前后关键帧视口与系数 t∈[0,1] 线性插值；
2. **只更新变化部分**：候选集为与 `union(v0,v1)` 相交的 box（任一中间视口都落在该并集内，集外 box 全程不可见且不会变化），每帧只重测候选并做有序集合差分，输出 added/removed/unchanged。

- 退化模式 `CULL_ANIM_FULL_REBUILD`：每帧对全部 n 个 box 全量重建，可见 id 与旧 `cull_run` 逐帧完全一致。
- 空输入 `n==0` 返回 `CULL_E_EMPTY`(-3)，`cull_strerror` 给出原因；t 越界、视口非法、未初始化等各有独立错误码。
- 基准与核对（`make -C cull verify`）：小图（6 box 手算场景）与大图（10 万 box 网格）各测增量/全量两模式，报告误差、耗时、重建次数、插值系数、候选数；轨迹导出 CSV 到 `build/benchout/`，由 `verify/check_traj.py` 独立重算并与 `verify/handcheck.csv` 手算表逐行核对。
