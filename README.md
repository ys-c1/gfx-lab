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

## cull 模块：两段式自适应视口剔除

边缘高速对象在旧的严格相交测试下会被误剔。cull 模块改为两段式（旧 API 保留且语义不变）：

1. `cull_apply_tolerance`：逐对象算自适应容差 `tol = tol_base + hypot(vx,vy)·dt`（像素）
2. `cull_run_adaptive`：对象盒按 `tol`、视口按全局 `margin` 各自外扩后做 AABB 相交；全零外扩时结果集与 `cull_run` 逐元素一致

输入校验：负容差/负 `dt`/负 `margin`、非有限速度（NaN/±Inf）返回 -3，并通过 `CullError` 指名字段（`tol`/`vx/vy`/`dt`/`margin`/`tolerance`）与对象下标。候选列表可由 `cull_export` 导出为每行一个 ID 的文本。

- 基准与报告：`make -C cull bench`（产物在 `artifacts/cull/`，含 `report.md`、逐对象 `details_*.csv`、`candidates_*.txt`、`summary.json`）
- 独立复算：`python3 -I tools/crosscheck.py artifacts/cull`（不链接 C 库，逐对象重算全部判定并核对导出文件，日志 `artifacts/cull/crosscheck.log`）
