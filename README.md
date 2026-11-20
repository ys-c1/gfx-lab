# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| shadow | C11 | demo.pgm | 线段光源阴影图，可选的子样本覆盖率累积 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

shadow 模块的演示与覆盖率分析：

```
make -C shadow demo          # 生成 demo.pgm（采样关闭，与原图逐像素相同）
./shadow/build/demo 4 out.pgm    # 每像素 4 个子样本
./shadow/build/demo measure      # 64/256 分辨率下 off、4、16 与解析覆盖率对比
```
