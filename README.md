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

## atlas 多页打包

`atlas_book_pack` 两阶段工作：先按页上限（page_w × page_h）切页，再逐页回填页内偏移；单页能放下时布局与 `atlas_pack` 完全一致。块大于页返回 `-2` 并经 `err_item` 给出矩形序号；`n<=0`（页数为零）返回 `-3`。`atlas_book_pack_greedy` 为高降序首次适配的贪心切页，用于对照页数与占用率。`atlas_book_export` 导出页表（每页占用率、每个矩形的页内偏移）到 `demo.txt`。

切页后的加载顺序：页号升序（0..page_count-1），页内按矩形原始序号升序。
