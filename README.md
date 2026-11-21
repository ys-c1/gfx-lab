# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo_*.ppm | 线段阴影、光源色两段式着色与两档性能基准 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## shadow：光源色阴影

- 两段式管线：先按光源色算阴影基调 `base = light.rgb * (1 - strength)`，
  再按逐像素遮挡比例 `o∈[0,1]` 混到画面 `out = frame*(1-o) + base*o`。
- 开关 `ShTune.enable=0` 时走旧的灰度路径，输出与原实现逐字节一致。
- 三种取色源（显式 RGB / 预设 / 色卡）互斥，同时配置返回 `SH_ECONFLICT`
  并通过 `sh_tune_check` 输出冲突两方名称。
- 低速（逐线段穷举）与高速（按每段阴影楔形标记粗格、只测候选线段）两档，
  遮挡数学完全相同、输出逐像素一致；`make -C shadow bench` 输出两档耗时、
  色调误差、混合比例、像素数、换色前后 CIE76 色差，并与独立穷举参考实现核对。
