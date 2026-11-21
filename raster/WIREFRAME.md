# 线框叠加：独立的叠加层与三段式合成

## 结论

- 线框不再直接写进颜色缓冲，而是有自己的叠加层（每层一张覆盖掩码 + 合并后的叠加图），所以**先画的填充永远盖不住线框**。渲染顺序无论是"填充 → 线框"还是"线框 → 填充"，结果一致。
- 合成严格分三段，各有独立入口：

  | 段 | 函数 | 写什么 | 不写什么 |
  |---|---|---|---|
  | 1 描边 | `rt_wire_stroke()` | 该层的掩码、统计 | `rgb`、`z`、其他层 |
  | 2 按层号合并 | `rt_wire_merge()` | 叠加图（覆盖、预乘颜色、alpha） | `rgb`、`z` |
  | 3 按不透明度混合 | `rt_wire_blend()` | `rgb` | `z` |

- **不启用时输出保持原样**：所有层 `enabled == 0`（或根本没有层）时三段都不写 `rgb`/`z`。`make -C raster demo` 生成的 `demo.ppm` 与改动前基线 SHA-256 完全相同，为 `ea571daa…257b`。
- **非法值一律拒绝并说明位置**：错误结构给出层号、已存储槽位、字段名、边号与两个顶点号；`rt_wire_errfmt()` 输出如 `layer 3: opacity; opacity must be finite and within [0,1]`、`layer 3: edge; edge 0 (v0-v1): coincident vertices`。被拒绝的层不占用槽位、不改变任何缓冲。
- **叠加顺序可复现**：先按层号升序，同层号再按描边先后（存储序号）。`RWireMergeStats` 回报完整顺序 `order[]`、`order_layer[]`、`order_opacity[]`、`order_covered[]`；两次相同描边得到逐字节相同的画面（bench 中由 FNV-1a 哈希与 `memcmp` 证明）。
- **与暴力遍历对照**：bench 另写一份不共用任何库代码的全帧遍历（每像素 × 每层 × 三条边、双层循环、`qsort` 排序、双精度合成），两个分辨率下逐像素颜色差 **0**，最大通道误差 **0**。

## 三段各自的语义

### 第 1 段：按边函数描边

距离用填充阶段同一个边函数：

```
e(p) = (b-a) × (p-a) = (b.x-a.x)(p.y-a.y) - (b.y-a.y)(p.x-a.x)
|e| / |ab| = 像素中心到该边所在直线的距离
```

投影参数 `t` 决定最近点是否落在线段内；落在两端之外时改用端点的欧氏距离，所以线端是圆头，不会把边当作无限长直线。像素中心为整数 + 0.5，覆盖判据是 `距离 <= halfwidth`，线宽名义值为 `2 * halfwidth`。

边 `i` 永远是 `v[i] → v[(i+1)%3]`，**不因为三角形是顺时针而换序**，因此顺时针三角形的边同样能描出来（填充阶段仍然只填一个环绕方向——demo 里第二个三角形填充像素为 0，但线框覆盖 783 像素）。

### 第 2 段：按层号合并

覆盖是 source-over，预乘颜色避免任何顺序下出现"先除后乘"的精度问题：

```
a_out = a_src + a_dst * (1 - a_src)
c_out = c_src * a_src + c_dst * (1 - a_src)
```

顺序键为 `(layer, seq)`：层号小的先合，同层号按描边先后。第 2 段只写叠加图，重复调用得到相同结果；描新的一层会让旧叠加图失效，必须重新合并才能混合。

### 第 3 段：按不透明度混合

```
out = round(overlay_pm + dst * (1 - overlay_alpha))
```

一次性从双精度四舍五入到 8 位，中间不落整。叠加图在成功后即被消费，重复调用 `rt_wire_blend()` 不会重复混合。`z` 从不读取也从不写入。

## 接口

```c
int rt_wire_stroke(RTarget *t, const RTri *tri, const RWireSpec *spec,
                   RWireStrokeStats *stats, RWireErr *err);   /* 段 1 */
int rt_wire_merge (RTarget *t, RWireMergeStats *stats, RWireErr *err); /* 段 2 */
int rt_wire_blend (RTarget *t, RWireBlendStats *stats, RWireErr *err); /* 段 3 */
int rt_wire_resolve(RTarget *t, RWireResolveStats *stats, RWireErr *err); /* 2+3 */
void rt_wire_clear(RTarget *t);
int rt_wire_edge_dist(const RVert *a, const RVert *b, int x, int y, double *dist);
const char *rt_wire_strerror(int code);
int rt_wire_errfmt(const RWireErr *err, char *buf, size_t cap);
```

`RWireSpec`：`layer`（>= 0）、`opacity`（有限且 ∈ [0,1]）、`rgb`、`halfwidth`（有限且 >= 0）、`enabled`。
`enabled == 0` 时整层是空操作，连校验都不做（便于关掉线框时不因旧数据报错）。

`RT_WIRE_MAX_LAYERS`（64）是同时存在的层数上限；超出返回 `RT_WIRE_ERR_CAPACITY`，不会部分写入。

## 拒绝的非法值与位置

| 条件 | 返回 | 位置信息 |
|---|---|---|
| `layer < 0` | `RT_WIRE_ERR_LAYER` | `layer` 字段，`field="layer"` |
| `opacity` 为 NaN/±inf/越界 | `RT_WIRE_ERR_OPACITY` | 层号 + `field="opacity"` |
| `halfwidth` 为 NaN/±inf/负 | `RT_WIRE_ERR_HALFWIDTH` | 层号 + `field="halfwidth"` |
| 顶点坐标非有限 | `RT_WIRE_ERR_VERTEX` | 层号 + `vertex=i` |
| 两顶点重合 | `RT_WIRE_ERR_DUP_VERTEX` | `edge=i`、`vertex=i`、`vertex2=(i+1)%3` |
| 边长 < `RT_WIRE_MIN_EDGE`(1e-6) | `RT_WIRE_ERR_SHORT_EDGE` | 同上 |
| 层槽位用尽 | `RT_WIRE_ERR_CAPACITY` | `field="layers"` |
| NULL 目标/三角形/配置/顶点 | `RT_WIRE_ERR_ARG` | — |

被拒绝时：不分配掩码、不占用槽位、`t->layer_count` 不变、`rgb`/`z` 不变（测试逐项断言）。

## 64×64 与 512×512 实测

场景 6 层：`L0 a=1.00`、`L0 a=0.50`（同层号，验证同层按描边先后）、`L1 a=0.25`、`L2 a=0.75`、`L2 a=0.60`、`L3 a=0.00`（全透明，只贡献覆盖不贡献颜色）。底层填充为暗色渐变，使混合有非黑目标色。

| 项目 | 64×64 | 512×512 |
|---|---|---|
| 各层覆盖（库/暴力） | 740/740, 442/442, 203/203, 374/374, 234/234, 297/297 | 5952/5952, 3577/3577, 1645/1645, 3035/3035, 1864/1864, 2598/2598 |
| 合并覆盖 / 贡献总数 / 不透明像素 | 1829 / 2290 / 740 | 18217 / 18671 / 5952 |
| 混合像素 / 其中完全替换 | 1630 / 740（暴力 1630） | 15689 / 5952（暴力 15689） |
| 叠加深度 ≥1/≥2/≥3/≥4 | 1829 / 419 / 37 / 5 | 18217 / 441 / 13 / 0 |
| 与暴力遍历的误差 | 0 像素，最大通道差 0 | 0 像素，最大通道差 0 |
| 线宽（名义 4.00 px） | 实测 [4,5]，最坏误差 1.00 px | 实测 [4,5]，最坏误差 1.00 px |
| 耗时：描边 / 合并 / 混合 | 0.162 / 0.030 / 0.009 ms | 7.648 / 1.067 / 0.225 ms |
| 合计 vs 暴力全帧 | 0.202 ms vs 0.293 ms（1.45×） | 9.037 ms vs 17.090 ms（1.89×） |
| 每像素 | 49.2 ns vs 71.5 ns | 34.5 ns vs 65.2 ns |
| 可复现 | 两次运行帧与统计完全一致，哈希 `4386305e…132f` | 同上，哈希 `464ac52f…0b44` |

线宽说明：像素中心落在 1 px 网格上，4 px 名义宽在四个 1/4 像素对齐下的覆盖数是 [4,5]，这是掩码式光栅器能给出的真实包络；表中报的是这个包络，不是单一采样值。

叠加顺序复现：bench 除检查 `(layer, seq)` 单调外，还把排好序的层按升序重新描边一遍，得到逐字节相同的帧（`order replay ... yes`）。

## 复现

```
make -C raster test     # 3290 checks
make -C raster demo     # demo.ppm（关闭，与基线同哈希）+ demo_wire.ppm（三层叠加）
make -C raster bench    # 上面两张表，同时写入 bench.txt
```

## 接口变更

- `RTarget` 增加叠加层私有状态；`rt_init` 用 `memset` 清零，旧调用方无需改动。
- `rt_init`/`rt_free` 会增加/释放叠加层缓冲；`rt_free` 之后可直接重新 `rt_init`。
- `rt_triangle` 签名与返回值不变。
- 新增 `rt_wire_*` 系列，互不干扰：只做填充时不产生任何叠加层开销，只做叠加时不改变深度。
