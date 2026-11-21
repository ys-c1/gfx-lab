# gfx-lab

五个小模块，面向 2D 渲染与音频，每个模块独立可构建、自带测试与演示产物。

| 模块 | 语言 | 产物 | 内容 |
|---|---|---|---|
| raster | C11 | demo.ppm | 三角形光栅化与 z 缓冲 |
| mip | C11 | demo.ppm | 盒式金字塔与纹理采样 |
| atlas | C11 | demo.txt | 序列图集货架打包 |
| mixer | C11 | demo.wav | 总线混音与增益 |
| cull | C11 | demo.txt（桶表 build/demo_buckets.txt） | 视口剔除与空间哈希两段式查询 |

每个模块：`make -C <模块> test`，演示：`make -C <模块> demo`。

## cull 空间哈希两段式剔除

对象多了以后逐个比包围盒太慢。`cull` 新增两段式路径，原 `cull_run` 作为对照基准保留：

1. `cull_hash_build`：把每个包围盒插入它覆盖的所有网格桶，桶内按对象下标升序保存；
2. `cull_hash_query`：只遍历视口覆盖的桶，桶内对象去重后逐个做重叠测试，输出按下标升序。

`cull_run_hashed` 是两段合一的入口；`opts.legacy != 0` 时完全关闭新分支，直接走 `cull_run`，
输出逐项照旧（顺序、数量、id 全部相同）。两段式的可见集合与逐对象比较**完全相同**：
盒与视口重叠时必然共用一个桶，闭区间网格不会漏掉恰好压线的对象（测试里逐项核对）。

- 统计与输出：每次查询报告网格桶数、非空桶、插入数、冲突数、冲突率（百分比，两位小数）、
  候选数与访问桶数；冲突数定义为 `sum(桶内对象数 - 1)`，冲突率为 `冲突数 / 插入数`。
  `cull_hash_export_buckets` 导出可复现的纯文本桶表（按 `(bucket_y, bucket_x)` 升序，
  桶内按对象下标升序）。
- 上限：桶总数与插入数各有上限（`CULL_HASH_MAX_BUCKETS` / `CULL_HASH_MAX_ENTRIES`，可经
  `CullHashOpts` 覆盖）。超限返回 `CULL_ERR_HASH_LIMIT`，`CullHashError.limit` 给出上限值，
  `field` 指出是 `"buckets"` 还是 `"entries"`，`message` 带具体数值。

```
make -C cull test     # 1131 checks
make -C cull demo     # 两种场景 + build/demo_buckets.txt
make -C cull bench    # 1千 / 1万对象，写 artifacts/cull_hash_*
python3 -I cull/verify_hash.py artifacts   # 纯标准库独立参考复核
```
