# shadow

2D 阴影图：点光源照射若干线段遮挡物，逐像素计算"是否被遮挡"与距离衰减。

## 原始接口

```c
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
```

单光源、整幅重绘。行为与引入多光源接口之前完全一致。

## 多光源：按强度排序、再按序叠加

多光源分两段：

| 阶段 | 做什么 | 代码 |
|---|---|---|
| 1. 排序 | 把光源按强度从亮到暗排成一张顺序表 | `sh_lights_order()` |
| 2. 叠加 | 按这张顺序表，逐像素依次叠加每个光源的阴影贡献 | `sh_render_lights()` |

```c
int sh_lights_order(const ShLight *lights, int nlight,
                    ShLightOrder *order, ShError *err);

int sh_render_lights(ShadowMap *m, const ShLight *lights, int nlight,
                     const ShSeg *segs, int nseg, int enable_multi,
                     ShMultiStats *stats, ShError *err);
```

顺序**只由光源的数值决定**，与调用方数组的排列无关：强度降序；强度相同则按 `x`
升序，再按 `y`、`radius` 升序；四个字段全等的两个光源再按输入下标区分——它们贡献的
数值完全相同，所以这一层不会改变结果。因此同一组光源无论以什么顺序传入，输出缓冲
逐位相同（`bench` 里对 5 个光源逐一旋转验证）。

叠加时按顺序**边算边累加**，不开每光源临时图：内存只有目标图加顺序表。

单光源在本路径下与旧接口逐位相同（`sh_render_lights(..., nlight=1, enable_multi=1)`
与 `sh_render()` 逐字节一致）。

### 关闭多光源时逐字节相同

`enable_multi == 0` 只渲染 `lights[0]`，走的是原来 `sh_render()` 的代码路径：

```c
sh_render_lights(&m, lights, n, segs, nseg, 0, &st, &err);
```

- 缓冲与 `sh_render(&m, &lights[0], segs, nseg)` **逐字节相同**；
- 场景 `sh_render(&m,&l,segs,2)` 的 `demo.pgm` SHA-256 与改动前基线一致：
  `bcaa7c8d36ffb3ec6729cbf2c39039ea1918c091f90df88c41cd67e18adcf031`；
- 缓冲 FNV-1a（128×128，demo 场景，`l={64,10,40,1}`）为 `63918a3aac7ab7fa`，与基线一致；
- `stats->passes == 1`、`stats->accumulate_ops == w*h`、`temp_maps == 0`。

注意：`enable_multi==0` 时仍然**校验全部**光源与遮挡物，参数不合法同样会被拒绝。

### 类型/取值不符：拒绝并指出字段名

C 语言没有运行期类型信息——结构体字段的静态类型是固定的。这里的"不符"指取值
不符合字段的声明域，或指针/尺寸参数形状不对。所有拒绝都返回负码，`ShError.field`
给出字段名（含数组下标）：

| 情况 | 返回码 | `field` 示例 |
|---|---|---|
| `radius <= 0` | `SH_ERR_LIGHT` (-4) | `lights[1].radius` |
| `intensity < 0` | `SH_ERR_LIGHT` | `lights[0].intensity` |
| 任一光源字段为 NaN/±Inf | `SH_ERR_LIGHT` | `lights[1].x` |
| 遮挡物字段为 NaN/±Inf | `SH_ERR_SEG` (-5) | `segs[1].x1` |
| `nlight == 0` | `SH_ERR_NOLIGHT` (-3) | `nlight` |
| `nlight > SH_MAX_LIGHTS` | `SH_ERR_ARG` (-2) | `nlight` |
| `nseg < 0` | `SH_ERR_ARG` | `nseg` |
| 必需指针为 NULL | `SH_ERR_NULL` (-1) | `lights` / `m` / `segs` / `order` |
| 顺序表写文件失败 | `SH_ERR_IO` (-6) | `path` |

`ShError.msg` 附带具体数值，如 `must be > 0 (0)`。校验在任何写入之前完成，
**失败的调用不改动目标图**（测试里逐像素确认）。

## 顺序表导出

```c
int sh_write_order_table(const char *path, const ShLightOrder *order, int n,
                         const char *scene, int w, int h);
```

输出 CSV，`make -C shadow demo` 写 `demo_order.csv`，`make -C shadow bench` 写
`bench_order_256.csv` 与 `bench_order_1024.csv`：

```
# scene=demo w=128 h=128 passes=2
rank,index,intensity,x,y,radius
0,1,1,40,12,40
1,0,0.400000006,100,8,26
```

`index` 是调用方数组里的下标，`rank` 是叠加顺序。统计结构 `ShMultiStats.order[]`
里也带同一张表，另有 `passes`、`accumulate_ops`（= 像素数×叠加次数）、各类字节数。

## 实测：256 与 1024

`make -C shadow bench`。场景 5 个光源（输入强度 `0.30 1.00 0.70 0.70 0.05`，
故意乱序，其中两个 0.70 构成平局）、4 条遮挡线段，几何按分辨率等比缩放。

### 排序结果与强度

| rank | 输入下标 | 强度 | 说明 |
|---|---|---|---|
| 0 | 1 | 1.00 | |
| 1 | 3 | 0.70 | 与 rank 2 同强度，`x` 小者在前 |
| 2 | 2 | 0.70 | |
| 3 | 0 | 0.30 | |
| 4 | 4 | 0.05 | |

两档分辨率的顺序表完全相同（坐标随分辨率缩放，但相对位置与强度不变）。

### 叠加次数、耗时、内存

| 分辨率 | 叠加次数 passes | 像素 | 累加操作数 | 耗时 | 每像素每遍 | 目标图 | 顺序表 | 峰值额外内存 | 省下的临时图 |
|---|---|---|---|---|---|---|---|---|---|
| 256×256 | 5 | 65 536 | 327 680 | 1.5 ms | 4.7 ns | 256.0 KiB | 100 B | 100 B | 1.2 MiB |
| 1024×1024 | 5 | 1 048 576 | 5 242 880 | 25.3 ms | 4.8 ns | 4096.0 KiB | 100 B | 100 B | 20.0 MiB |

"省下的临时图"= 每光源一张临时图（`nlight × w × h × 4` 字节）本实现不分配。
峰值额外内存只有顺序表；累加直接写进目标图（先清零）。

### 误差：与独立逐像素计算对照

对照程序是**另一份实现**：从光源列表按排好的顺序、用 double 精度重算每个像素的
遮挡判定与衰减，不调用库内任何函数。

| 分辨率 | 最大误差 | 平均误差 | RMSE | 与顺序无关性 |
|---|---|---|---|---|
| 256×256 | 1.20e-07 | 1.74e-08 | 2.44e-08 | 4 种旋转全部逐位相同 |
| 1024×1024 | 1.37e-07 | 1.75e-08 | 2.45e-08 | 4 种旋转全部逐位相同 |

误差就是 float 与 double 的舍入差（约 1 ulp），说明两段式的顺序与叠加都正确。

### 为什么以前会"忽明忽暗"

旧用法是在循环里对每个光源调用 `sh_render()`。`sh_render()` 会**覆盖**目标图，
所以最后画的那个光源独占画面——基准里确认这个序列的结果与"只用最后一个光源"
逐字节相同；把输入顺序反过来，画面最大变化 0.283（0..1 量程）。光源顺序一乱，
谁在最后就变，于是画面忽明忽暗。

另一种"顺序叠加"的写法（逐光源加到同一缓冲）虽然不再丢光源，但 float 加法不满足
结合律：按输入顺序算与按强度顺序算，在 256 档有 1 075 / 65 536 个像素、在 1024 档
有 17 266 / 1 048 576 个像素的最低有效位不同（最大差 1.19e-07）。两段式把顺序钉死
在光源数值上，这类抖动随之消失。

## 测试

`make -C shadow test`（513 项）：

- 旧接口回归：遮挡/未遮挡取值、非法光源返回 `-2`；
- 排序：5 光源场景的完整顺序表；顺序表是输入的一个排列；
- **排列不变性**：4 个光源的全部 24 种输入排列逐一渲染，缓冲与基准逐字节相同；
  两个字段全等的光源互换后仍逐字节相同；
- 平局规则：同强度按 `x` 升序，导出 CSV 的 rank 0/1/2 人工核对；
- 单光源多光源路径 == 旧接口逐字节；`enable_multi==0` == `sh_render(lights[0])` 逐字节；
- 逐像素 double 对照：全部像素误差严格为 0（测试场景含遮挡与多光源重叠）；
- 拒绝路径：`radius`、`intensity`、NaN/Inf、`segs`、`nlight`、`nseg`、空指针逐一
  检查返回码与 `field` 字符串；失败调用不改动目标图；
- 顺序表 CSV：表头、场景行、rank 0 行内容；非法参数返回码。

`make -C shadow bench` 另外验证：5 光源旋转全部逐位相同、旧循环序列等于最后光源、
`enable_multi==0` 与 `sh_render()` 逐字节一致，并导出两档顺序表。
