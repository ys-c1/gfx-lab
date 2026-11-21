# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt / 偏移表·索引表 | 序列图集货架打包 + 多图集合并（两段式偏移与索引改写） |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## atlas 多图集合并

解决多张图集合并后索引错乱。两段式，顺序不可颠倒：

1. `atlas_merge_layout`：先算各源图集的新偏移（`base`，按源顺序连续编号）并产出全局货架布局。`do_merge=0` 时布局照旧——各源独立货架带、互不重叠，索引走恒等改写。
2. `atlas_rewrite_indices`：按 `new_idx = base + old_idx` 改写旧索引，并检测：
   - 偏移越界返回 `-4`（源号或局部块号非法）；
   - 索引冲突返回 `-5`（两条引用落到同一全局槽位）。
   - 出错时 `err_at` 给出条目序号，`err_slot` 给出被重复占用的槽位；超宽块由阶段一返回 `-2` 并给全局块序号。

度量：占用率（used/canvas）、货架数、占用带高、改写条数与耗时。
导出：`atlas_export_offsets`（偏移表）、`atlas_export_indices`（索引表），均为制表符文本。
`make -C atlas demo` 在 128/512 两宽度及非合并模式下输出逐块手算对照，并生成 `demo_offsets_*.txt`、`demo_indices_*.txt`。
