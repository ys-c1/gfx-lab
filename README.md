# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集打包：顺序货架 + 最大矩形装箱 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## atlas 装箱

`atlas_pack` 保留原来的顺序货架算法，布局与旧版逐像素一致；`atlas_pack_maxrect`
/ `atlas_pack_job` 是新的三段式装箱：

1. 维护最大矩形空位表（每放一块把相交空位切成最多四块，并去掉被包含的空位）；
2. 每块按底层、再靠左打分，零度和九十度两种朝向都试，择优放置；
3. 按原序号回填坐标，统计条带高度、旋转块数和浪费率 `1 - 面积/（宽度×高度）`。

错误码带序号：图块两个朝向都宽于条带 → `ATLAS_ERR_TOO_BIG`；空位表为空 →
`ATLAS_ERR_EMPTY`；有空位但放不下 → `ATLAS_ERR_NO_FIT`；非法朝向提示或固定朝向
放不下 → `ATLAS_ERR_ORIENT`；尺寸非正 → `ATLAS_ERR_RECT`（`-2`，与旧接口一致）。

`atlas_waste_lower_bound` 给出任何装箱都达不到的下界（面积与最长短边），
`atlas_waste_upper_bound` 给出 NFDH 保证的上界。`make -C atlas demo` 在相同输入上
对比 128 与 512 宽条带的浪费率（含贪心对照）、条带数、耗时和上下界。
