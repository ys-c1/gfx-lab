# shadow

2D 软阴影图。光线按段（`ShSeg`）遮挡，每像素输出一个 `[0,1]` 光照值。

## 问题

旧实现每像素只在中心点采一次样（`sh_render`）。倾斜阴影边缘压在规则像素
网格上时，遮挡判定在相邻像素间整列翻转，形成像素级锯齿——即**规则网格纹
（走样）**，周期为 1 个像素。

## 两段式抖动采样

`sh_render_jit` 把渲染显式拆成两段：

1. **扰动**（`jittered_spot`）：把像素均分成 `tile*tile` 个子格，采样点原在
   子格中心；按抖动矩阵给每个点加一个限制在本子格内的偏移
   （幅度 ±1/(2·tile)），保持分层采样不重叠的性质。
2. **平均**（`average_jittered`）：对扰动后的每个采样点逐个做段遮挡判定与
   衰减计算，再取平均。规则采样的一个二元硬台阶被拆成多个小台阶，软边
   像素因此得到 0/1 之间的灰阶。

矩阵类型：

| 类型 | 说明 |
|---|---|
| `SH_JITTER_NONE` | 不抖动 |
| `SH_JITTER_BAYER` | 有序 Bayer（2x2 / 4x4 / 8x8 递归生成） |
| `SH_JITTER_ROTATED` | 位反转置换版 Bayer，打散行列方向周期性 |
| `SH_JITTER_CUSTOM` | 调用方通过 `cfg.mat` 提供矩阵 |

`SH_JITTER_ROTATED` 对标准 Bayer 的线性索引做**位反转置换**——这是双射，
保证每个矩阵槽位恰好被写一次（无重复、无遗漏），同时把行列方向的规则低频
结构换成高频分散排列。

档位（采样点数 = `tile²`）：

| 档位 | tile | 每像素采样点 |
|---|---|---|
| `SH_TIER_FAST`（低速） | 2 | 4 |
| `SH_TIER_HIGH`（高速） | 8 | 64 |

## 开关与冲突

`ShJitterCfg.enabled == 0`（或传入 `NULL`）时走与 `sh_render` 逐像素完全
一致的单点路径，**行为不变**，回传采样点数为 1。

`sh_jitter_validate` 对自相矛盾的配置返回负码，并在错误信息里点名冲突的
两方，例如：

- `enabled=1 vs matrix_type=SH_JITTER_NONE (two sides disagree on jitter)`
- `matrix_type=SH_JITTER_CUSTOM vs mat=NULL (custom type demands a matrix)`
- `tier=SH_TIER_FAST vs tile=8 (FAST requires tile=2)`

## 矩阵导出

`sh_jitter_export(path, type, tile, mat)` 写成带 magic 头 `SHJIT1` 的文本，
首行给出 type/tile/count，其后每行一个 `[0,1)` 偏移（行主序），可直接回读
核对。导出前校验取值范围，拒绝 NaN 与越界值，并把负零归一化（不写出
`-0.000000`）。`demo` 会导出两档矩阵：`demo_jitter_2x2.txt`、
`demo_jitter_8x8.txt`。

## 基准与核对

`make bench`（`bench.c`），场景为 45° 倾斜遮挡段（最能暴露锯齿网格纹）：

- **穷举参考**：每像素 32×32 个均匀细采样直接平均（1024 点/像素），作为真值；
- 对规则采样、低速 Bayer、低速位反转、高速 Bayer 四种情形分别报告
  每像素采样点数、耗时（ms）、相对真值的 RMSE 与最大误差；
- **网格纹强度（step）**：边缘带内最大的单像素台阶幅度。规则二元采样的
  硬台阶最大，抖动把它拆分、削平；
- **晶格走样能量（lattice）**：取每列竖直最大台阶组成 1D 序列，去掉斜边
  线性包络后的残差 RMS；
- **网格纹周期（periodPx）**：残差 DFT 在晶格频段（周期 2~4px）主峰对应的
  空间周期（像素）。

核对项（全部通过才返回 0）：

1. 规则采样 lattice 显著高于穷举参考，且主峰落在 2~4px 晶格周期；
2. 高速抖动把最大台阶削到规则采样的 85% 以下；
3. 两档抖动相对穷举真值的 RMSE 都小于规则采样的一半；
4. 高速档晶格能量向穷举参考收敛（低于规则采样的 80%）。

## 命令

```
make -C shadow test   # 单元测试（开关、两段平均、冲突报错、矩阵双射、导出边界）
make -C shadow demo   # 输出规则/两档抖动 PGM 与抖动矩阵文本
make -C shadow bench  # 误差、耗时、网格纹强度与周期、穷举核对
```
