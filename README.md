# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化、z 缓冲与透视校正纹理插值 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## raster 透视校正

斜看地面时屏幕空间重心权重是仿射的，纹理会被拉歪。`rtt_triangle(t, tri, correct)` 分三段处理：

1. 逐顶点求透视倒数 `1/w`；
2. 用校正权重 `(b_i/w_i)/Σ(b_j/w_j)` 插值 `u,v,z`（`correct=0` 时退化为纯屏幕空间仿射权重，与旧逐像素路径逐位一致）；
3. `rtt_unproject` 对每个像素在相机空间做射线-三角形反算，作为插值结果的逐像素真值核对。

顶点级校验（错误信息带顶点号）：深度 `w<=0` 返回 `RT_ERR_WZERO`，坐标非有限返回 `RT_ERR_FINITE`，三角形退化成点/线返回 `RT_ERR_DEGEN`。

基准与核对：`make -C raster bench`，在 64 与 256 两档分辨率下分别列出 UV 最大/平均误差、填充像素数、覆盖不一致数与单次耗时；校正模式 UV 误差约 1e-16，仿射模式约 0.5，反算覆盖率核对为 0 不一致。棋盘格产物 `bench_correct.ppm` / `bench_affine.ppm`。
