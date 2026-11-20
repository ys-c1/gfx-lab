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

## shadow 软边（softshadow）

`shadow/src/softshadow.{h,c}` 在硬阴影图上做三段式软边：

1. **距离场算半径** `sb_distance_field`：逐像素取到最近遮挡线段的距离，
   半径 = `blur_radius_min + max(0, 距离-dist_begin)*dist_scale`，封顶 `blur_radius_px`。
2. **可分离高斯模糊** `sb_blur_separable`：先水平后垂直两次一维卷积；
   全局核按最大半径生成，小半径像素取中心段并重归一，采样次数写入 `sample_count`。
3. **合成并测宽** `sb_composite`（`enable=0` 时原样拷贝，画面照旧）+
   `sb_measure_soft_edge`（亚像素 10%~90% 过渡带宽度）。

校验（均返回负错误码，`sb_error_msg` 指出字段名）：`blur_radius_px==0` →
`SB_ERR_RADIUS_ZERO`；核长（字段 `kernel_len=2*r+1`）为偶 → `SB_ERR_KERNEL_EVEN`。
边界处理 `SB_BORDER_CLAMP`（边缘像素重复，默认）/ `SB_BORDER_ZERO`（补 0）。
核可用 `sb_kernel` / `sb_kernel_export` 导出。

- 测试（含与朴素二维卷积的逐像素手算对照）：`make -C shadow soft-test`
- 演示（64/256 两分辨率，输出软边宽度误差、采样次数、耗时三栏）：`make -C shadow soft-demo`
