# gfx-lab

小模块集合，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt | 视口剔除与排序 |
| ninepatch | C11 | demo_*.ppm | 九宫格缩放：四角直拷、四边拉伸/平铺 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## ninepatch 九宫格

解决按钮等图块整体拉伸后边框变厚变形的问题。`np_render` 把图块切成九宫：

- **四角**：1:1 直接逐像素拷贝，永不缩放；
- **上/下边**：水平方向按 `h_strategy` 拉伸（`NP_STRETCH`）或平铺（`NP_TILE`）；
- **左/右边**：垂直方向按 `v_strategy` 拉伸或平铺；
- **中心**：双向拉伸填充。

`NpConfig.enabled = 0` 时退化为整图最近邻缩放，输出与不开九宫格的布局逐像素一致。

错误码（返回负值并对应具体字段）：

| 返回 | 含义 | 涉事字段 |
|---|---|---|
| -1 | 空指针或尺寸非法 | src/cfg/dst、w/h/dw/dh |
| -2 | 边宽超图块或切分越界（边宽为负、左右之和大于图块宽、上下之和大于图块高） | left/right/top/bottom |
| -3 | 策略非法（非 STRETCH/TILE） | h_strategy/v_strategy |

