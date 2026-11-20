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

## cull 运动感知剔除（两阶段）

快速移动对象在帧间跨越视口边界时，朴素静态剔除会漏剔。`cull` 模块新增：

1. `cull_expand_boxes`：按运动方向扫掠扩展包围盒，每对象单侧扩展像素
   `ex = ratio*speed*|dx|/len`、`ey = ratio*speed*|dy|/len`，写入 `CullMotionReport`；
2. `cull_run_swept`：按扩展后盒做视口剔除；
3. `cull_motion`：两阶段合一。`ratio<=0` 时退化为原静态剔除，集合与旧接口逐对象一致。

错误约定（序号为 0 起下标，同时可换算“第 idx+1 个”）：`speed<0` 返回
`CULL_E_NEG`；需扩展但扩展为零（`speed==0` 或零方向向量）返回 `CULL_E_ZERO`，
出错下标经 `err_index` 给出。

- 基准：`make -C cull bench`（1千 / 1万对象，报漏剔数、误剔数、中位数耗时）
- 独立复核：`python3 -I cull/verify_cull.py artifacts`（纯标准库参考实现，
  与 C 落盘结果逐对象对照；`ratio=1.0` 漏剔为零）
