#!/usr/bin/env python3
"""独立参考实现：不依赖 objtab.c，按公开约定重新核对开放寻址表的布局。

用法:
    python3 -I tools/refcheck.py build/snapshot_inc_10000.txt [更多快照...]

快照格式（每行）:
    # tag <名称> slots <槽位数> entries <条目数>
    S <槽位下标> <key> <value>
    E <槽位下标> <墓碑标记 0=空 2=墓碑>

核对内容（只依赖快照本身，不需要插入历史）:
  1. 槽位数是 2 的幂，记录的下标都在容量内；
  2. 条目数、键唯一性、槽位状态互斥；
  3. 每个键按「murmur3 finalizer + 线性探测、遇空槽停止」的规则可达——
     这条同时校验了哈希函数与探测语义，是查表正确性的充分条件；
  4. 没有键落在探测链被空槽截断的位置上。

退出码 0 表示全部一致。
"""

import sys


def murmur_finalizer(x: int) -> int:
    """与 objtab.c:hash_u32 相同的 murmur3 finalizer。"""
    x &= 0xFFFFFFFF
    x ^= x >> 16
    x = (x * 0x7FEB352D) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x846CA68B) & 0xFFFFFFFF
    x ^= x >> 16
    return x


def parse(path):
    tag = None
    slots = None
    entries = None
    items = {}      # slot -> (key, value)
    tombs = set()
    empties = set()
    max_slot = -1
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                parts = line.split()
                for i, tok in enumerate(parts):
                    if i + 1 >= len(parts):
                        break
                    if tok == "slots":
                        slots = int(parts[i + 1])
                    elif tok == "entries":
                        entries = int(parts[i + 1])
                    elif tok == "tag":
                        tag = parts[i + 1]
                continue
            parts = line.split()
            kind, idx = parts[0], int(parts[1])
            max_slot = max(max_slot, idx)
            if kind == "S":
                items[idx] = (int(parts[2]), int(parts[3]))
            elif kind == "E":
                st = int(parts[2])
                (tombs if st == 2 else empties).add(idx)
            else:
                raise SystemExit(f"{path}: 未知记录类型 {kind}")
    if slots is None:
        raise SystemExit(f"{path}: 缺少槽位数字段")
    return tag, slots, entries, items, tombs, empties, max_slot


def check(path):
    tag, slots, entries, items, tombs, empties, max_slot = parse(path)
    problems = []

    if slots <= 0 or (slots & (slots - 1)) != 0:
        problems.append(f"槽位数 {slots} 不是 2 的幂")
    if slots > 0 and max_slot >= slots:
        problems.append(f"快照槽位下标 {max_slot} 超出容量 {slots}")
    if entries is not None and entries != len(items):
        problems.append(f"头注明条目数 {entries} 与 S 记录数 {len(items)} 不符")

    keys = [k for (k, _v) in items.values()]
    if len(set(keys)) != len(keys):
        problems.append("存在重复键")
    if set(items) & tombs:
        problems.append("同一槽位既是条目又是墓碑")
    if set(items) & empties:
        problems.append("同一槽位既是条目又是空槽")
    if tombs & empties:
        problems.append("同一槽位既是墓碑又是空槽")
    if slots > 0 and len(items) + len(tombs) + len(empties) != slots:
        problems.append(
            f"槽位状态未覆盖全表: S={len(items)} T={len(tombs)} E={len(empties)} != {slots}")

    if slots > 0:
        mask = slots - 1
        for slot, (key, _v) in items.items():
            i = murmur_finalizer(key) & mask
            found = False
            for _ in range(slots):
                if i in items and items[i][0] == key:
                    found = True
                    break
                if i in empties:
                    break
                i = (i + 1) & mask
            if not found:
                problems.append(f"key {key} 在参考探测链上不可达（应位于槽位 {slot}）")

    status = "OK" if not problems else "FAIL"
    print(f"{status} {path} tag={tag} slots={slots} entries={len(items)} "
          f"tombs={len(tombs)} empties={len(empties)}")
    for p in problems[:20]:
        print(f"   - {p}")
    if len(problems) > 20:
        print(f"   ... 共 {len(problems)} 个问题")
    return not problems


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    ok = True
    for path in argv[1:]:
        ok = check(path) and ok
    print("ALL CONSISTENT" if ok else "INCONSISTENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
