# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo.pgm | 2D 软阴影与斜率偏移去自阴影 |

本轮 shadow 改动：按表面斜率给遮挡线段算深度偏移，并把它放进射线求交里（命中点离接收像素不足偏移量就不遮挡）。偏移量为 0 时渲染结果与改动前逐位相同。

```c
ShSeg segs[1] = {{20, 30, 110, 50}};
ShLight light = {65, 2, 60, 1};
ShadowMap m;  ShError err;
sh_init(&m, 128, 128);
sh_render_ex(&m, &light, segs, 1, &err);      /* 偏移默认开启 */
ShAcne a;
sh_acne(&m, &light, segs, 1, &a, &err);       /* 条纹统计 */
```

`sh_set_bias(&m, bias, clamp, &err)` 里 `bias` 是每单位斜率的像素偏移，`bias = 0` 即关闭。同一世界换分辨率用 `sh_init_scaled(&m, w, h, 世界单位/像素)`，像素偏移保持不变。`bias` 为负、或斜率/端点不是有限值时返回负值，并在 `err.field` 写明字段（如 `bias`、`segs[1].slope`）。

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

