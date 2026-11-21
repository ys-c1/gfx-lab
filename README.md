# gfx-lab

小模块集合，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| asset | C11 | demo.gpak | 资源打包与校验 |
| shadow | C11 | demo.pgm | 点光源硬阴影 / 矩形面光软阴影（三段管线） |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## shadow：矩形面光软阴影

`sh_render` 保留原有点光源硬阴影行为（开关为假时逐位不变）。面光走
`sh_render_ex` 的三段管线：

1. **按光源朝向取多点** `sh_sample_quad`：在以光源中心为原点、半宽 `aw`
   半高 `ah` 的发光面上生成 `cols x rows` 个采样点，按 `angle` 旋转到世界
   坐标；局部采点分布可经 `sh_local_point` 单独取到。
2. **按覆盖比例合成** `sh_composite`：每像素追踪全部采样点，
   `coverage = 未遮挡点数 / 总点数`，亮度 = 距离衰减 × coverage。
3. **报半影宽度** `sh_penumbra`：扫描覆盖图，统计部分覆盖（0<cov<1）
   过渡带的平均/最大像素宽度。

档位（`sh_preset_cfg`）：`OFF`（点光源旧路径）、`FAST` 高速 4×4=16 点、
`SLOW` 低速 12×12=144 点、`EXHAUST` 穷举参考 48×48=2304 点。面光与档位
开关冲突时返回 `-3`，`sh_last_error()` 的信息指明冲突两方（如
`light.aw/ah ... vs cfg.gear=OFF`）。

`make -C shadow bench` 对照穷举档记录两档的耗时、覆盖比例 RMSE/最大误差、
半影宽度与平均覆盖比例，并把四个朝向（0、π/4、π/2、π）的采点表（局部 +
世界坐标）写到 `shadow/build/sample_table.tsv`。
