# grid

均匀网格空间划分，带两段式合并，解决相邻对象被切到两个格子、重复登记导致的重复绘制问题。

## 两段流程

1. **第一段（重叠插入 `grid_build`）**：每个对象包围盒与哪些格子重叠就登记到哪些格子。
   横跨格线的对象会在多个格子各登记一次，`grid_duplicates()` 统计的就是这种重复登记数。
2. **合并（`grid_merge_groups`）**：按包围盒重叠关系做并查集合并（含传递闭包，
   A 碰 B、B 碰 C 则三者同组），重算每组的合并 AABB。
3. **第二段（唯一归属 `grid_assign_unique`）**：每个合并组只分配给锚点（左上角）所在的
   唯一一个格子，每组只调度一次，重复数按构造即为 0。

## 上限与报错

| 上限 | 值 | 超限返回 |
|---|---|---|
| `GRID_MAX_OBJECTS` | 100000 | `-2`，`grid_last_limit()` 给出 `GRID_MAX_OBJECTS=100000` |
| `GRID_MAX_CELLS`（cols×rows） | 1000000 | `-3`，给出 `GRID_MAX_CELLS=1000000` |

其它返回：`-1` 参数/分配失败，`-4` 网格几何非法，`-5` 包围盒非法（w/h ≤ 0）。
demo 命中上限时以退出码 3 结束并在 stderr 打印上限值。

## 构建与运行

```sh
make test          # 单元测试（23 项）
make demo INPUT=data/small.txt COLS=25 ROWS=20
bash tools/run_compare.sh   # 两规模 + Python 参考工具对照 + 上限检查
```

demo 输入每行一个盒子：`x y w h [layer]`，`#` 开头为注释。

## 输出指标（每规模均打印）

- 两段各自的 `error`、`time`（秒）、`dup`（重复登记数）、`cand`（候选/格登记总数）
- 合并步骤的 `groups`（合并后组数）、`pairs`（重叠对数）、`multi_groups`、`max_group`
- 合并前后差异：对象数、重复数、候选数的 before → after 与 delta

`tools/ref_tool.py` 是独立 Python 实现（`python3 -I` 运行），`run_compare.sh`
会对 n、groups、merge_pairs、dup1/2、cand1/2 逐项交叉校验。
