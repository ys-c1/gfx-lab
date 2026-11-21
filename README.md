# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲，可选两段式深度预通道 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## raster 深度预通道

`RConfig.enable_prepass` 关闭（默认）时走历史单遍循环，画面逐像素一致；打开后分两段：

1. **预通道**：只做覆盖测试和深度写，不碰颜色缓冲，同时快照进入测试前的旧深度；
2. **颜色遍**：按快照旧深度拒绝被遮像素，只写颜色（`depth_eps=0` 时与单遍位精确一致）。

参数越界由 `rt_config_validate` / `rt_set_config` 返回 `-3`，并通过 `err_field` 指明字段
（`enable_prepass`、`track_stats`、`depth_eps`）。`track_stats=1` 时 `RStats` 累计
覆盖评估数、深度命中、被拒像素、颜色写入及两遍各自耗时。`make -C raster demo`
输出 32×32 与 256×256 两档基准（含被拒像素数、预通道耗时、深度命中率与两遍画面差异）。
