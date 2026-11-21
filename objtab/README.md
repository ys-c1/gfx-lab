# objtab — 对象表的分段扩缩容

开放寻址哈希表（线性探测，容量 2 的幂），解决的问题是：对象规模变化时整表重建
会造成明显卡顿。这里把一次重建拆成**两段**：

1. **规划**：`ot_plan_resize()` 按新规模算出新槽位数与扩容倍数；超限直接报错并给出上限值。
2. **迁移**：`ot_resize_begin()` → `ot_migrate_step(budget)` × N → `ot_resize_finish()`，
   每步只扫描固定数量的旧槽位，把一次大停顿摊成多小步。

## 迁移期间的语义

| 操作 | 行为 |
|---|---|
| `ot_get` | 先查新表、未命中再查旧表 → **迁移前后命中逐项一致** |
| `ot_export` | 新表已迁入 + 旧表未扫描部分，合起来正好是全部逻辑条目（无重无漏） |
| `ot_put` / `ot_del` | 返回 `OT_E_MIGRATING`，不产生半迁移状态 |
| `ot_resize_rollback` | 旧表迁移期间**一行未改**，丢弃新表即精确回滚，误差严格为 0 |
| `ot_resize_finish` | 仅在旧表扫描完且迁移条数对得上时才释放旧表 |

旧路径 `ot_resize_full()`（整表重建）保留，两条路径对同一批条目产出**同一槽位布局**
（`ot_layout_checksum` 相等），可直接对照。

## 规模与倍数怎么算

```
new_slots = next_pow2(ceil(new_scale / load_factor))     load_factor 默认 0.70
growth    = new_slots / old_slots
```

`new_scale` 是「期望容纳的对象数」，必须 ≥ 当前条目数，否则 `OT_E_RANGE`。

## 错误码

| 码 | 含义 | 附加信息（`OtError`） |
|---|---|---|
| -1 `OT_E_ARG` | 空指针 / 参数非法 | — |
| -2 `OT_E_NOMEM` | 分配失败 | — |
| -3 `OT_E_FULL` | 表已满（内部，正常不会外泄） | — |
| -4 `OT_E_LIMIT` | **超限** | `limit_slots` / `limit_entries` / `requested_*` |
| -5 `OT_E_MIGRATING` | 迁移期间禁止写入 | — |
| -6 `OT_E_STATE` | 状态非法（未在迁移 / 未迁完就 finish） | — |
| -7 `OT_E_NOTFOUND` | 键不存在 | — |
| -8 `OT_E_RANGE` | 新规模小于当前条目数 | — |
| -9 `OT_E_STALE_PLAN` | 计划生成后表已变化 | — |

上限有两道：`OtOptions.max_entries`（条目数）与 `max_slots`（槽位数）。
新增条目一超 `max_entries` 就返回 `OT_E_LIMIT`；规划时任一上限被越过也返回
`OT_E_LIMIT`，`OtError` 里带 `limit_*` 上限值与 `requested_*` 计划值。

## 构建与核对

```
make test     # 单元测试：规划/倍数、超限、迁移中一致性、回滚、两条路径对照、随机压力
make demo     # 4000 对象：规划 → 41 步迁移 → 中途查询 → 回滚 → 超限报告
make bench    # 1 万 / 10 万两档，整表重建 vs 两段增量迁移，写 build/snapshot_*.txt
python3 -I tools/refcheck.py build/snapshot_*.txt   # 独立参考工具复核槽位布局
```

`tools/refcheck.py` 不依赖 `objtab.c`：它按公开约定（murmur3 finalizer + 线性探测、
遇空槽停止）从快照重建每个键的查找路径，逐槽校验可达性与状态互斥。

## 实测（本机 gcc 15 -O2）

两档规模，新规模均取当前条目数的 4 倍：

| 档 | 条目 | 槽位 | 扩容倍数 | 迁移条数 | 整表耗时 | 增量耗时 | 增量分批 / 单步上限 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 10,000 | 16,384 → 65,536 | ×4.00 | 10,000 | 0.224 ms | 0.111 ms | 32 步 / 351 条 |
| 2 | 100,000 | 262,144 → 1,048,576 | ×4.00 | 100,000 | 4.344 ms | 1.770 ms | 512 步 / 244 条 |

两条路径逐项核对（两档均为 0）：迁移前后**命中差 0**、**导出内容误差 0**；
迁移一半回滚后**槽位布局逐槽相同、导出内容逐项相同**；增量与整表**最终布局校验和一致**。
`tools/refcheck.py` 对 4 个快照全部 `OK`。

增量总耗时并不比整表慢，而单步工作量有硬上界（≤ `budget` 条），
把一次 10 万条的停顿拆成 512 次各 ≤ 244 条的小停顿——这正是去掉卡顿的关键。
