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

## atlas 超限分册（实验，默认关闭）

一张图集超过显存上限时拆成若干册（每册一张可独立驻留的图集纹理）。老货架打包器
`atlas_pack` 一行未改；新路径在 `atlas/src/atlas_vol.{h,c}`，开关 `ATLAS_USE_VOL` 默认 0，
关闭时老路径的 `test`/`demo` 输出逐字节不变。

```
make -C atlas test        # 老路径回归（6 项）
make -C atlas test-vol    # 新路径 93 项检查（含手算数值）
make -C atlas demo-vol    # 两档倍率统计 + 册表导出 + 与老路径比对
make -C atlas verify      # 三者全跑
```

- **两段式**：阶段一 `avol_split` 按每册上限（`page_w`/`page_h` 与 `cap_bytes`）只切册；
  阶段二 `avol_layout` 逐册用与 `atlas_pack` 相同的货架规则回填册内坐标。
- **老路径**：`avol_pack(..., enable_new=0)` 直接调用 `atlas_pack`；单册时新路径的册内坐标
  与导出册表都和老路径逐字节相同（test 用 `tmpfile` 全量比对）。
- **越界检出**：`avol_texel`/`avol_lookup`/`avol_validate`/`avol_probe` 越界返回
  `AVOL_ERR_OOB`，`bad_index` 报出矩形序号（册级不一致报 `-(册号+1)`）。
- **两档倍率**：坐标与页尺寸用 1x 点，`scale=2` 只放大物理像素与字节；同一 `cap_bytes`
  下每册装的矩形更少，册数上升。
- **统计**：册数、每册占用（`used_px`/`used_bytes`/`shelves`/`occupancy`）、索引长度
  （`index_len`/`index_bytes`）、逐像素解析次数 `samples`、统计误差 `waste_px`/`waste_bytes`。
- **册表导出**：`avol_export` 输出确定性文本（`demo_vol_1x.txt`、`demo_vol_2x.txt`）；
  `demo-vol` 把每个统计值与纸笔推导值并排打印（`got`/`paper`/`OK|MISMATCH`）。
