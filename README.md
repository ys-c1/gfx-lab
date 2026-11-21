# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo.pgm / build/*.csv | 点光源阴影，及面光源（条带）方向性软阴影：方向掩码、方向加权、三段合成 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

shadow 模块额外提供 `make -C shadow bench`：以 64 样本穷举为真值，对低/高两档分别记录掩码误差、图像 RMSE、耗时、方向档数、采样数与射线数，并把方向掩码表导出为 CSV（`build/mask_low.csv`、`build/mask_high.csv`、`build/mask_exhaustive.csv`）与穷举结果核对。`ShShapeCfg.enable` 为假时走旧点光源路径，输出与 `sh_render` 逐位一致；互斥配置冲突返回 `-3`，错误信息经 `sh_errmsg()` 指明冲突的两方。
