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

## raster 两阶段深度早退

`rt_render(t, tris, n, enable_early, &st)` 把渲染拆成两段：

1. **深度提前判可见**（`rt_tri_visible`/`probe_triangle`）：只做边测试与只读深度比较，不写任何缓冲；
2. **只给通过者着色**（`shade_triangle`）：探测为 0 像素的三角形整体跳过，统计进 `RStats.tris_early_out`。

`enable_early=0` 时每个三角形直接走第二阶段，z/rgb 缓冲与开启早退逐字节相同（demo 每次运行做 `memcmp` 校验）。错误约定：

- 顶点深度非有限（inf/nan）→ 返回 `-3`，stderr 与 `st.error_index` 指出三角形号和顶点号；
- 目标缓冲为空（z/rgb 为 NULL）→ `rt_render` 返回 `-4` 并指出三角形号；单三角形入口返回 `-1`；
- 退化三角形（面积 < 1e-12）→ 跳过并计入 `tris_degenerate`。

`make -C raster refcheck` 用独立 Python 参照实现（`tools/refcheck.py`，不读 C 的任何输出）逐三角形手算边测试/深度/写入像素，在 32 与 256 两档分辨率下与 C 端 `RStats` 全字段对照，并对 PPM 做跨语言逐字节 `cmp`。
