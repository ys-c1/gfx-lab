# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo32/demo256.ppm | 三角形光栅化、z 缓冲、近平面裁剪 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## raster 近平面裁剪

两阶段管线：`rt_clip_triangle` 先按半空间 `z >= near_z` 用 Sutherland–Hodgman
把三角形裁成小块并扇形拆分，统计裁剪产生的新顶点数；再对每块走与
`rt_triangle` 共用的同一个光栅化循环。

- `RTarget.clip` 默认 0（关闭）、`near_z` 默认 0；关闭时跨近平面的三角形
  直接以 `RE_NEAR` 拒绝（即"整块消失"的旧行为），存活场景画面与旧实现逐字节一致。
- 错误码：`RE_NEAR`（顶点深度小于近平面）、`RE_DEGEN`（顶点重合/零面积）、
  `RE_EMPTY`（裁剪后无剩余）；`RStats.err_tri` 给出首个出错三角形的编号。
- `make -C raster demo`：32 与 256 两档分辨率对比裁剪顶点数、填充像素数、每帧耗时。
- `make -C raster handcheck`：Python 独立逐顶点手算；
  `make -C raster nearcheck`：贴脸三角形在裁剪开/关下的对比。
