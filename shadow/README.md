# shadow

2D 阴影图：点光源照射若干线段遮挡物，逐像素计算"是否被遮挡"与距离衰减。

原始渲染只有一个入口：

```c
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
```

## 按投射距离截断：问题与分两段的做法

原来的做法是"无限远投射"：一条遮挡物会把它背后的整条射线全部压黑，不管多远。
场景里遮挡物一多，远处就堆满互相叠加的阴影，看起来全是噪点，而且每像素要测
**全部**线段。

现在分成两段，用"光到遮挡物的距离"做截断：

| 段 | 接口 | 做什么 |
|---|---|---|
| A 截断 | `sh_truncate()` | 把每条线段按光距裁到 `cutoff` 以内；跨越距离分档边界的地方切开。整段都在 `cutoff` 外的直接丢弃。 |
| B 重投 | `sh_render_truncated()` | 只用 A 产出的**截断后线段**重投阴影。光距超过 `cutoff` 的接收点不再被遮挡。 |

一段到底：`sh_render_cast()` 把两段串起来，A 的统计可经 `cast_out` 取回。

为什么截断不改变画面（在"截断语义"下是精确的）：遮挡物 D 要挡住接收点 P，
D 必须落在光 L 到 P 的连线上，于是 `|L-D| < |L-P|`。所以截掉 `|L-D| > cutoff`
的遮挡物，不可能改动任何 `|L-P| <= cutoff` 的像素；而 `|L-P| > cutoff` 的像素按定义
本来就不该再变黑。B 的结果与"直接用原始线段逐像素算、但同样按 cutoff 关阴影"
完全一致——`bench` 里用一份不调用库的双精度求值器逐像素对照，差异只有 float 舍入量级
（max |Δ| ≈ 6e-8；8 位 PGM 上 0~4 个字节差 1 级）。

距离**分档**：`cutoff` 以内按接收点到光的距离再分 `tier_count` 段，每段有自己的阴影亮度
`bright[k]`（0 = 全黑，和原实现一样；1 = 完全不压暗）。档界由 `split[0..tier_count-2]`
给出。A 阶段切分点取在每条分割圆周与线段的交点上，所以"截断段数"就是各档内的段数之和。

截断是**闭合**的：把截断后的段再截一次，得到同样的划分（段数相同、端点差 ≤ 1ulp）。
相邻两段共用同一个端点（右边段的左端点就是左边段的右端点的同一对 float），所以既不
重叠也不留缝。

## 关闭时与旧版本逐字节相同

`enable = 0` 时 `sh_render_cast()` **直接调用**原来的 `sh_render()`，不截断、不分配
cast set、不做额外统计。输出的 `ShadowMap` 与旧二进制逐位相同，因此 `demo.pgm` 的
SHA-256 不变：

```
$ make demo && sha256sum demo.pgm
bcaa7c8d36ffb3ec6729cbf2c39039ea1918c091f90df88c41cd67e18adcf031  demo.pgm
```

`test` 里对同一场景同时跑旧接口与关闭态的 `sh_render_cast()`，`memcmp` 逐位比较；
`bench` 的 off 档也做同样比较并打印 `identical to legacy`。

## 类型不符要拒绝，并指出字段名

C 没有运行时类型，所以由调用方声明值的类型，字段自己记着应有的类型；不一致就拒绝，
并回填字段名：

```c
typedef enum { SH_TY_NONE = 0, SH_TY_INT = 1, SH_TY_FLOAT = 2 } ShType;

int sh_cfg_set(ShTruncCfg *cfg, ShField f, ShType ty, const void *value, ShError *err);
```

| 字段 | 类型 | 合法范围 |
|---|---|---|
| `enable` | int | 0 / 1 |
| `tier_count` | int | 1..4 |
| `cutoff` | float | 有限且 > 0 |
| `split_0..2` | float | 有限且 ≥ 0；须严格递增且 < cutoff（跨字段校验） |
| `bright_0..3` | float | 有限且在 [0,1] |

出错时 `err.code` 是 `SH_ERR_TYPE` / `SH_ERR_RANGE` / `SH_ERR_FIELD` / `SH_ERR_NULL` /
`SH_ERR_ORDER`，`err.field` 指向静态字段名（如 `"cutoff"`、`"bright_2"`），`err.msg`
带上具体数值。类型/范围错误当场拒绝、**不写入**该字段。`sh_error_clear()` 清空诊断。

示例（`test.c`）：

```c
int iv = 8;
sh_cfg_set(&c, SH_F_CUTOFF, SH_TY_INT, &iv, &e);   /* == SH_ERR_TYPE        */
/* e.field == "cutoff", c.cutoff 未被修改                 */
```

## 接口一览

```c
void sh_cfg_default(ShTruncCfg *cfg);          /* enable=0, tier_count=1, bright=0 */
int  sh_cfg_validate(const ShTruncCfg *cfg, ShError *err);
int  sh_cfg_set(ShTruncCfg *, ShField, ShType, const void *value, ShError *);

int  sh_truncate(ShCastSet *out, const ShLight *, const ShSeg *, int nseg,
                 const ShTruncCfg *, ShError *);          /* 段 A */
void sh_castset_free(ShCastSet *);

int  sh_render_truncated(ShadowMap *, const ShLight *, const ShCastSet *,
                         const ShTruncCfg *, ShRenderStats *, ShError *); /* 段 B */

int  sh_render_cast(ShadowMap *, const ShLight *, const ShSeg *, int nseg,
                    const ShTruncCfg *, ShCastSet *cast_out,
                    ShRenderStats *, ShError *);          /* A + B，关闭时直达旧接口 */
```

`ShCastSet` 报 A 阶段：输入段数、截断后段数、丢弃段数、被切开的段数、被裁端的段数、
各档段数、截断前后遮挡物总长。
`ShRenderStats` 报 B 阶段：各档像素数 / 被遮挡像素数 / 平均亮度，以及 `cutoff` 之外的
像素数。`sh_render_truncated()` 会检查 cast set 与配置的 `tier_count/cutoff/split`
一致；只有 `bright` 允许与截断时不同（换配色不用重截）。

## 测试

`make -C shadow test`（176 项检查）：

- 旧接口 6 项回归原样保留；
- 关闭态与旧 `sh_render()` 逐位相同，且不分配 cast set；
- 类型不符（float 字段传 int、int 字段传 float、`SH_TY_NONE`）逐一拒绝并核对字段名；
- 范围错误与跨字段顺序错误（未递增、越过 cutoff）同样报字段名，且不改动配置；
- A 阶段统计自洽（各档段数之和 = 总段数、所有段都在 cutoff 内、远段被丢弃、空输入合法）；
- 解析长度：切线、穿出圆周、退化点等情形与手算值对照；
- B 阶段与独立逐像素求值器对照，4 个场景 × 2 种分辨率全部 0 像素超差；
- 每档亮度都真的出现在输出里，且 cutoff 之外确实没有阴影；
- 截断闭合：再截一次段数不变、内容逐字节相同；重复调用结果一致。

另外用 ASan/UBSan 跑过全部测试（`-fsanitize=address,undefined`），无报错。

## 基准

`make -C shadow bench`，256 与 1024 两档，每个用例在独立子进程里跑（RSS 为该例自身的
`VmHWM`）。场景 10 条遮挡物，光在顶部。每个用例报：距离档位、截断段数、各档段数、
亮度、各档像素/遮挡数/平均亮度、与独立求值器的误差、内存、耗时，以及**截断前后**单独
一张对照表。

### 截断段数（A 阶段）

256：

```
config                     cutoff(px)  tiers(px)          in pieces drop split  每档段数
off (legacy)                -           -                 10   -     -     -     -
1 tier, cutoff .60          154         [0 154]           10   7     3     0     7
2 tiers, cutoff .60         154         [0 77 154]        10  10     3     3     4/6
3 tiers, cutoff .60         154         [0 56 108 154]    10  13     3     5     3/5/5
3 tiers, short cutoff .40   102         [0 38 72 102]     10  11     5     4     2/5/4
4 tiers, cutoff .60         154         [0 38 72 113 154] 10  16     3     5     2/5/4/5
```

1024（档位按比例放大，段数与 256 相同——几何随分辨率等比缩放）：

```
config                     cutoff(px)  tiers(px)              in pieces drop split  每档段数
1 tier, cutoff .60          614         [0 614]               10   7     3     0     7
2 tiers, cutoff .60         614         [0 307 614]           10  10     3     3     4/6
3 tiers, cutoff .60         614         [0 225 430 614]       10  13     3     5     3/5/5
3 tiers, short cutoff .40   410         [0 154 287 410]       10  11     5     4     2/5/4
4 tiers, cutoff .60         614         [0 154 287 451 614]   10  16     3     5     2/5/4/5
```

遮挡物总长：256 上 834.1 px → 508.3 px（cutoff .40 时 → 276.5 px）；
1024 上 3336.4 px → 2033.1 px（→ 1105.9 px）。

### 亮度与各档统计（B 阶段）

1024，每格 `像素数/被遮挡/平均亮度`：

```
config                      总平均亮度 被遮挡   cutoff外   档0                 档1                 档2                 档3
off (legacy)                0.1294    774623   0        -
1 tier, cutoff .60          0.2358    350801   439880   1048576/350801/0.236
2 tiers, cutoff .60         0.2865    350801   439880   185716/72277/0.395  862860/278524/0.263
3 tiers, cutoff .60         0.3040    350801   439880   107050/30084/0.508  236170/133222/0.294 705356/187495/0.276
3 tiers, 短 cutoff .40      0.3438    145834   734890   55428/5563/0.687    108662/54774/0.379  884486/85497/0.318
4 tiers, cutoff .60         0.3189    350801   439880   55428/5563/0.687    108662/54774/0.351  209984/121376/0.323 674502/169088/0.282
```

亮度档 `bright = 0 / 0.35 / 0.75`（3 档）或 `0 / 0.25 / 0.55 / 0.85`（4 档）。
档 0 平均亮度高，是因为它离光最近、衰减最强以外的像素本来就亮；把 cutoff 缩短
（.40）会让留在画面里的阴影更集中在近处，整体平均亮度反而更高（0.3438）。

### 误差（对照逐像素计算的结果）

独立求值器：双精度、直接用原始线段、同一 cutoff 与档位规则，不调用库。

```
config                      max abs    mean abs   rmse      >1e-5  >1/255  8位差字节/最大级差
off (legacy)                8.4e-08    8.4e-08    8.4e-08   —      —       —         （与旧实现逐位相同）
256: 每一档                 ≤5.96e-08  ≤1.0e-08   ≤1.8e-08  0      0       0/0
1024: 3 tiers, cutoff .60    5.96e-08   9.2e-09    1.7e-08   0      0       4/1
1024: 其余各档              ≤5.96e-08  ≤1.0e-08   ≤1.8e-08  0      0       0/0
```

即：两段式管线在它自己的语义下与逐像素计算**一致**，剩下的只是 float 舍入。
唯一一处 8 位差 1 级的 4 个字节，是某个像素恰好落在 float 求值器与库的 rounding 边界上
（连续值差 6e-8，量化后翻了一级）。

### 内存

```
size   map B       pieces B   accounted   rss base   rss peak   rss delta
256    262144      224..512   262656      980K       1816K      836K
1024   4194304     224..512   4194816     988K       9504K      8516K
```

`sizeof(ShCastPiece) = 32 B`。截断产物极小（10 条线最多 16 段，512 B），截断几乎不占内存；
RSS 的增长主要是 1 MB/4 MB 的阴影图本身与进程基线，与是否截断无关（off 档 1024 峰值
9440K，开启档 9504K，差 64 KB）。

### 耗时

```
size  config                     truncate   re-cast    legacy
256   off                        —          0.578 ms   0.578 ms
256   1 tier, cutoff .60         0.001 ms   0.400 ms   0.510 ms
256   3 tiers, cutoff .60        0.002 ms   0.628 ms   0.510 ms
256   3 tiers, 短 cutoff .40     0.001 ms   0.400 ms   0.510 ms
1024  off                        —          9.14 ms    9.14 ms
1024  1 tier, cutoff .60         0.001 ms   6.50 ms    8.14 ms
1024  3 tiers, cutoff .60        0.001 ms   9.88 ms    8.10 ms
1024  3 tiers, 短 cutoff .40     0.001 ms   6.92 ms    8.00 ms
1024  4 tiers, cutoff .60        0.002 ms   11.9 ms    8.24 ms
```

截断本身是微秒级（10 条线、每段解几个二次方程）。重投的耗时取决于**段数**：1 档和
短 cutoff 段数少，比旧实现快 20~35%；档数越多、切出的段越多，反而比旧实现慢
（4 档 16 段时慢约 45%）。也就是说，截断省下的是"远处遮挡物不参与测试"，
但每条遮挡物被切开后段数增加会吃掉这部分收益。若目标只是压掉远处噪点，
`tier_count = 1`（不切档）是最划算的。

### 截断前后（与旧版本对照，单独列出）

```
1024  config                    shadowed-  shadowed+   lum-     lum+     changed   max diff
      off                       774623     774623      0.1294   0.1294   0         0
      1 tier, cutoff .60        774623     350801      0.1294   0.2358   423822   0.3182
      2 tiers, cutoff .60       774623     350801      0.1294   0.2865   702346   0.3182
      3 tiers, cutoff .60       774623     350801      0.1294   0.3040   744539   0.3182
      3 tiers, 短 cutoff .40    774623     145834      0.1294   0.3438   769060   0.4118
      4 tiers, cutoff .60       774623     350801      0.1294   0.3189   769060   0.3306
```

`shadowed-` 为旧实现被遮挡的像素数（无 cutoff），`shadowed+` 为截断后的；
`changed` 为逐像素不同的像素数，`max diff` 为最大逐像素绝对差（值域 [0,1]）。
变化全部是**有意的**：一是 cutoff 之外不再压黑（`cutoff .40` 时被遮挡像素从 77 万
降到 14.6 万），二是 cutoff 内的阴影按档位提亮（全黑 → 0.35/0.75）。
若把 `bright` 全设 0 且 `cutoff` 放到图外，截断前后就应当回到逐位相同的旧画面
（`test` 与 `bench` 的 off 档覆盖了这条路径）。

## 说明与限制

- 语义按**接收点距离**截断：遮挡物在 `cutoff` 内才参与投射，`cutoff` 外的接收点不再变黑。
  这是"阴影投得太远"的直接解法；若想要别的截断口径（例如按遮挡物到光的距离决定
  它是否能挡住更远的点），需要另加规则。
- 落在遮挡物直线上（`t` 或 `u` 恰在边界）的像素本来就处于原实现的浮点刀锋上；
  `test` 的场景刻意避开这种退化位置，否则旧实现自身也会在两次运行间抖动。
- 每像素仍要遍历**截断后的**全部段；没有做空间加速结构。段数少时快，档多时慢，见上表。
- `ShCastPiece` 的端点由 double 算出后存为 float；重截一次时端点可能差 1ulp，
  划分与段数不变（`bench` 的 idempotent 栏按 ≤1e-3 px 判定）。
