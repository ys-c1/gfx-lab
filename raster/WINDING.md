# 光栅器环绕判定与背面剔除

## 结论

- 光栅器现在先算有向面积 `A2 = (v1-v0) × (v2-v0)`，`A2 > 0` 记为逆时针（CCW），`A2 < 0` 记为顺时针（CW），与 `RT_CULL_*` 开关配合决定画哪一面。
- 三个开关：`RT_CULL_KEEP_CCW`（只留逆时针）、`RT_CULL_KEEP_CW`（只留顺时针）、`RT_CULL_BOTH`（双面都画）。被剔除的面、被拒绝的退化三角形都按输入序号报出。
- **关闭剔除（`RT_CULL_NONE`）时逐像素与改动前完全相同**：`demo.ppm` 与改动前基线 SHA-256 均为 `ea571daa…257b`，且测试内嵌了一份改动前算法的逐字节副本做对照，颜色缓冲与深度缓冲全部逐位相同。

## 环绕判定

有向面积用顶点坐标直接算，**不依赖屏幕 y 轴方向**：

```
A2 = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x)
A  = A2 / 2
```

| 条件 | `RTWinding` | 处理 |
|---|---|---|
| `A2 > 1e-12` | `RT_WIND_CCW` | 按开关决定是否绘制 |
| `A2 < -1e-12` | `RT_WIND_CW` | 按开关决定是否绘制 |
| 两顶点 x、y 相同 | `RT_WIND_DEGENERATE` | 拒绝，理由 `coincident_vertices` |
| `A2 == 0` 但顶点不重合 | 同上 | 拒绝，理由 `zero_area` |
| `0 < |A2| < 1e-12` 或非有限 | 同上 | 拒绝，理由 `degenerate_winding` |

约定说明：`A2` 的符号取自调用方提供的坐标数值。如果调用方用图像坐标（y 向下），正面积在屏幕上看起来是顺时针——头文件里写明了这一点，标签永远指坐标数值的符号，不指屏幕观感。

## 开关语义

| 模式 | CCW 面 | CW 面 |
|---|---|---|
| `RT_CULL_NONE` | 画 | **不画（沿用旧行为）** |
| `RT_CULL_KEEP_CCW` | 画 | 剔除，理由 `backface_cw` |
| `RT_CULL_KEEP_CW` | 剔除，理由 `backface_ccw` | 画 |
| `RT_CULL_BOTH` | 画 | 画 |

`RT_CULL_NONE` 特意保留旧路径而不是"双面都画"，这样关闭剔除时图像逐像素不变；双面绘制请显式用 `RT_CULL_BOTH`。

## 统计与序号

`rt_batch()` 返回 `RTBatchStats`：

- `triangles` / `drawn` / `culled` / `rejected`
- `culled_idx[]`、`rejected_idx[]`：被剔除或被拒绝三角形的**输入序号**
- `reason_counts[RT_CULL_REASON_COUNT]`：按理由分类计数（`drawn`、`backface_cw`、`backface_ccw`、`coincident_vertices`、`zero_area`、`degenerate_winding`）
- `covered`：所有排序前被填充规则接受的像素中心数（重叠会重复计）
- `filled`：真正通过深度测试并写入的像素数
- `elapsed_ms`：批处理墙钟耗时

单个三角形的 `rt_triangle_info()` 另给 `winding`、`signed_area`、`signed_area2`、`covered`、`filled`。

## 32×32 与 128×128 实测

场景共 7 个三角形：2 个 CCW、1 个 CW、1 个顶点重合、1 个共线零面积、1 个亚阈值细条、再加 2 个与主体重叠的（CCW/CW 各一）。

### 32×32

| 模式 | drawn | culled | rejected | 填充像素 | 手算覆盖 | 手算按序填充 | 误差 | 耗时（21 次中位数） |
|---|---|---|---|---|---|---|---|---|
| none | 4 | 0 | 3 | 439 | 439 | 439 | +0 | 0.003 ms |
| keep-ccw | 3 | 1（序号 1） | 3 | 439 | 439 | 439 | +0 | 0.003 ms |
| keep-cw | 1 | 3（序号 0,5,6） | 3 | 213 | 213 | 213 | +0 | 0.001 ms |
| both | 4 | 0 | 3 | 572 | 652 | 572 | +0 | 0.004 ms |

### 128×128

| 模式 | drawn | culled | rejected | 填充像素 | 手算覆盖 | 手算按序填充 | 误差 | 耗时（21 次中位数） |
|---|---|---|---|---|---|---|---|---|
| none | 4 | 0 | 3 | 6783 | 6783 | 6783 | +0 | 0.039 ms |
| keep-ccw | 3 | 1（序号 1） | 3 | 6783 | 6783 | 6783 | +0 | 0.038 ms |
| keep-cw | 1 | 3（序号 0,5,6） | 3 | 3355 | 3355 | 3355 | +0 | 0.020 ms |
| both | 4 | 0 | 3 | 8908 | 10138 | 8908 | +0 | 0.059 ms |

两个分辨率下拒绝的序号都是 `2 3 4`，理由分类都是 `coincident_vertices=1 zero_area=1 degenerate_winding=1`。

说明：

- `none` 与 `keep-ccw` 的填充像素相同，因为关闭剔除时 CW 面本来就不画；两者只差"是否报告剔除了序号 1"。
- `both` 的 `手算覆盖 10138` 大于填充 8908，是因为重叠三角形被覆盖两次；`filled` 按像素计一次，和"按输入顺序带深度测试的手算"逐格吻合（误差 +0）。
- `keep-cw` 只有 1 个 drawn，是因为场景里另两个"额外"三角形也是 CCW。

### 有向面积误差

| 三角形 | 分辨率 | 库算出 A2 | 手算 A2 | 误差 |
|---|---|---|---|---|
| tri 0 | 32×32 | +695.250000 | +695.250000 | 0 |
| tri 0 | 128×128 | +11124.000000 | +11124.000000 | 0 |
| tri 1 | 32×32 | -418.750000 | -418.750000 | 0 |
| tri 1 | 128×128 | -6700.000000 | -6700.000000 | 0 |

手算值在 `bench.c` 里是字面量（`11124`、`-6700`，低分辨率按 `s²` 缩放），不是复用库自己的算式，所以是独立对照。

## 复现

```
make -C raster test     # 96 checks
make -C raster demo     # demo.ppm + 四种开关的统计
make -C raster bench    # 上面两张表（写入 bench.txt）
```

## 接口变更

- `RTarget` 增加 `cull` 字段；`rt_init` 默认 `RT_CULL_NONE`（旧行为）。
- 新增 `rt_clear`、`rt_set_cull`、`rt_get_cull`、`rt_classify`、`rt_triangle_info`、`rt_batch`、`rt_stats_free`、`rt_winding_name`、`rt_cull_reason_name`。
- `rt_triangle` 签名与返回值不变（返回写入像素数，退化返回 -2），旧调用方无需改动。
- `rt_init` 会清零 `cull`，但**不会**因结构体新增字段而改变旧代码的初始化方式（旧代码传入的是 `RTarget t;`，由 `rt_init` 填充）。
