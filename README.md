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

## atlas 引用计数图集（实验，默认关闭）

`atlas/src/atlas_ref.{h,c}` 新增一条按引用计数管理块、可两段式整块重置的图集路径；
老货架打包器 `atlas.c` 一字未改。开关 `ATLAS_USE_REFCOUNT` 默认 0，此时
`atlas_ref.c` 不参与编译，`make -C atlas demo` 的输出与 `atlas/testdata/demo_legacy.txt` 逐字节相同。

```
make -C atlas test          # 老路径回归
make -C atlas test-ref      # 新路径 257 项检查（含纸笔模型对照）
make -C atlas demo-ref      # 1x/2x 两档统计 + 重置前后布局导出 + 与老路径字节比对
make -C atlas verify        # 三者全跑
```

- **两段重置**：`atlas_ref_reset()` 先按引用计数标记空闲块（`reset_marked`/`reset_leaked`），
  再整页清零缓冲区并清空索引（`reset_index_cleared`）。仅释放到 0 的块也不会自动回收，
  只在下一次 reset 时统一回收。
- **越界检出**：`atlas_ref_probe()`/`upload()`/`page_pixels()`/`validate()` 越界返回
  `ATLAS_ERR_OOB`，`AtlasError` 里带块的编号（`index`）、页号（`page`）、坐标（`x`/`y`）
  与上限（`detail`），`oob_checks` 累计次数。
- **两档倍率**：点坐标与预算按 1x 记，`scale=2` 只放大像素与字节数（`page_px`、`*_bytes`）。
- **统计**：分配/请求/填充误差（`padding_points` == `stat_error`）、空闲、页内碎片
  （`slack_points`）、页数、索引长度、活跃/空闲块数、引用计数、命中/未命中采样次数、
  释放与归零次数、平衡校验（`balance_points`/`balance_bytes` 必须为 0）。
- **布局导出**：`atlas_ref_export()` 输出确定性文本；重置后的空图集与新建图集导出逐字节相同，
  `demo_ref` 同时把每个统计值与纸笔推导值并排打印（`got` / `paper` / `ok|MISMATCH`）。
