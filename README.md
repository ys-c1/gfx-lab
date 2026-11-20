# gfx-lab

小模块集合，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| tex | C11 | demo-atlas*.ppm | 导数 LOD + 三线性/各向异性采样，接图集 UV 链 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## tex 模块

三段式采样（`tex/src/tex.c`）：

1. **导数定层**：由屏幕导数算 texel 足迹 `rho`，`lambda = log2(rho_max) + bias`；
2. **长宽比拆子样本**：各向异性模式 `N = round(rho_max/rho_min)`（上限 8），沿长轴放 N 个探针，子样本层号减去 `log2(N)`；
3. **相邻两层双线性混合**：层内 4 点双线性，层间按 lambda 小数部分加权，三线性最多 8 taps。

采样器接收 `atlas_pack` 的图块矩形，把图块局部 UV 重映射为图集 UV，图集位图经 `mip_build` 构链。

- `bias_enabled=0` 走 legacy 路径，恒等图块直接调用 `mip_sample`，逐像素字节一致（test 全量比对）；
- 偏置/导数非有限、层号越界（`level`/`lod`）、图块越界（定位到 `tile_x/tile_y/tile_w/tile_h/atlas_w/atlas_h`）均返回负码并向 stderr 打印字段名；
- `tex_ref.c` 是独立参考采样器：只从基图层做足迹盒面积加权，不经过生产 LOD 逻辑；
- `make demo` 跑 2x/8x 倍率 × 128/512 图集宽度矩阵，输出 RMSE/PSNR、taps/px、耗时与三联 PPM。

