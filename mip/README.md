# mip

盒式金字塔与纹理采样。原有接口 `mip_build` / `mip_free` / `mip_sample` 行为与返回码不变。

## 两段式层采样

旧做法一次只能采一层，多层纹理要一层层切换。现在拆成两段：

```c
/* 第一段：按层号选图（带缓存） */
int mip_layer_select(MipLayerCache *cache, int level, MipLayer *out, MipError *err);
int mip_layer_select_list(MipLayerCache *cache, const int *levels, int count,
                          MipLayer *out, MipError *err);

/* 第二段：对同一位置批量采样已选好的层 */
int mip_layer_sample_same(const MipLayer *layers, int count, double u, double v,
                          unsigned char *out, MipError *err);

/* 两段合并的便捷入口，并累计统计 */
int mip_batch_sample(MipLayerCache *cache, const int *levels, int count,
                     double u, double v, unsigned char *out,
                     MipStats *stats, MipError *err);
```

`MipLayer` 是选定层的一份描述（层号、宽高、字节数、像素指针），指向 `MipChain` 内部，
生命周期跟随链。选层缓存是 `levels` 项的表，第一次选某层是 miss，之后同层都是 hit。
`mip_cache_set_enabled(cache, 0)` 可关闭缓存——结果字节不变，只是全部计为 miss。

**恢复默认：** 不传 `stats`、缓存默认开启、整数层号、`[0,1]` 内的 `u/v` 时，
`mip_batch_sample` 的每个输出都与 `mip_sample` 逐字节相同，取整用的是同一套截断 + 钳制公式。
测试覆盖全部层与多个位置（含 `u/v` 的 0、1、越界负值与大于 1 的值）；基准里 8000 / 80000 个
采样点逐字节对比，0 处不一致。

## 错误报告

所有新接口返回 `MIP_OK` 或负错误码，并通过 `MipError` 定位：

| 码 | 含义 | `field` | `index` / `level` |
|---|---|---|---|
| `MIP_ERR_ARG` | 空指针、数量为负、层结构非法 | 具体字段名 | 批量数组下标 / 层号 |
| `MIP_ERR_LEVEL` | 层号越界 | `"levels"` | **出错的数组下标**；`level` 为越界值 |
| `MIP_ERR_UV` | `u` 或 `v` 非有限 | `"u"` / `"v"` | -1 |
| `MIP_ERR_IO` | CSV 打不开或写失败 | `"path"` | -1 |
| `MIP_ERR_CACHE` | 缓存与链不匹配 / 未初始化 | `"cache"` / `"chain"` | 视情况 |
| `MIP_ERR_MEM` | 分配失败 | 具体字段名 | -1 |

层号出错时 `index` 指向**层号数组里的位置**（不是层号本身），`level` 给出那个非法值；
单层选择时两者相同。批量采样在整组选层成功前不写输出，失败时缓冲保持原样。
`cache`、`out`、`levels` 允许为 `NULL` 时按上表报错。

## 层表导出

```c
int mip_layer_table_build(const MipChain *c, MipLayerTable *t, MipError *err);
int mip_layer_table_write_csv(const MipLayerTable *t, const char *path, MipError *err);
```

CSV 表头 `level,width,height,bytes,checksum_fnv1a`，校验和为 FNV-1a 32 位，写成 `0x%08x`。
`mip_checksum` 单独导出，便于外部逐项核对。

## 测试

`make -C mip test`（156 项检查），除旧接口回归外覆盖：

- 默认配置下与 `mip_sample` 逐字节一致（全部层 × 7 个位置，含越界 `u/v`）；
- 关闭缓存后仍逐字节一致；
- 选层结果与 `MipChain` 内部指针/尺寸/字节数一致；
- 错误码 + 字段名 + 数组下标/层号：批量中间项越界、批量首项负数、单层越界、
  空指针、负计数、`NaN`/`inf` 的 `u`/`v`、坏层结构、CSV 路径不可写；
- 统计口径：层数、`selects`/`hits`/`misses`/`samples`/`batches`，含同批重复层号与关缓存；
- 层表内容与独立重算的 FNV-1a 校验和一致，CSV 行数/表头正确；
- 出错后缓存仍可继续使用。

## 基准

`make -C mip bench`，512×512 源图、10 层、每次批量 8 层、同一位置；
1000 与 10000 次批量采样两档，每段计时取 5 轮最小值。

参考核对（全部在本文件内独立实现，不调用 mip 的选层/采样逻辑）：

- `ref_build()` 重造盒式金字塔，逐层与 `MipChain` 比对字节；
- `ref_sample()` 独立点采样公式，与 `mip_sample`、两段式输出逐字节比对；
- 层表逐层核对尺寸、字节数、FNV-1a 校验和，并导出 `bench_layers.csv`。

```
image 512x512, levels 10 (512x512 .. 1x1), 8 layers per batch
layer table: 10 levels match reference (size, bytes, checksum)
layer table: wrote bench_layers.csv (10 rows)

+-------+--------+--------+--------+--------+---------+---------+---------+---------+---------+-----------+--------+
| scale | levels | layers | samples| hits   | misses  | select  | sample  | batch   | legacy  | mismatch  | max    |
|       |        | /batch |        |        |         | ms      | ms      | ms      | ms      | samples   | err    |
+-------+--------+--------+--------+--------+---------+---------+---------+---------+---------+-----------+--------+
|    1k |     10 |      8 |   8000 |   99.8% |      10 |   0.028 |   0.019 |   0.053 |   0.029 |         0 |      0 |
|   10k |     10 |      8 |  80000 |   99.9% |      10 |   0.279 |   0.233 |   0.572 |   0.306 |         0 |      0 |
+-------+--------+--------+--------+--------+---------+---------+---------+---------+---------+-----------+--------+
```

误差全部为 0：两段式与旧接口、旧接口与本文件内的独立公式都逐字节相同。

耗时分两种用法：

```
Per-pixel batches (select once per batch):        Select once, reuse across positions:
+-------+---------+---------+---------+           +-------+---------+---------+---------+
| scale | batch   | legacy  | speedup |           | scale | tiled   | legacy  | speedup |
+-------+---------+---------+---------+           +-------+---------+---------+---------+
|    1k |   0.053 |   0.029 |   0.54x |           |    1k |   0.020 |   0.029 |   1.48x |
|   10k |   0.572 |   0.306 |   0.53x |           |   10k |   0.233 |   0.300 |   1.28x |
+-------+---------+---------+---------+           +-------+---------+---------+---------+
```

- **每像素重新选层**（左手表）：两段式比旧接口慢约 2 倍。选层本身有成本（查缓存、
  填 `MipLayer`、统计），每批只采一个位置时摊不回来。这是预期结果，不是回归。
- **选一次层、复用多个位置**（右手表）：1.2–1.5 倍加速。这才是两段式的目标用法：
  层集合在多个像素/多个位置上不变，只是 `u/v` 变。
- 两档规模的单位成本基本持平（10k 约为 1k 的 10 倍），说明没有随采样数增长的开销。

> 计时数字依赖机器与编译器。上表是 WSL2 / gcc 13 `-O2` 的一次结果；换机器请以
> `make -C mip bench` 的实际输出为准。误差、层表核对与"逐字节一致"不依赖计时。

## 说明与限制

- `MipLayer.pix` 指向链内部数据，`mip_free` 之后失效；不要在链释放后继续用选定层。
- 缓存按层号做全有/全无的记录，不跟踪链内容变化。链重建后请重新 `mip_cache_init`。
- 第二段是逐层点采样：每层各自按自己的分辨率算纹素坐标。不做层内双线性，也不做
  mip 层间混合——与旧接口保持一致。
- `u/v` 为 `NaN`/`inf` 时报 `MIP_ERR_UV`；有限但极端的值按旧公式钳到边缘纹素，
  不会发生越界的 `double -> int` 转换。
