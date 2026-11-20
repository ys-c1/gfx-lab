# asset — GPAK 资源包与版本迁移

## 包格式（全部小端、定长字段）

### v1（历史格式）

```
magic[4]="GPAK" | version u16=1 | count u16
重复 count 次: name[32] | size u32 | offset u32
data_size u32 | data[data_size]
```

### v2（当前格式）

```
magic[4]="GPAK" | version u16=2 | count u16
重复 count 次: name[32] | size u32 | offset u32 | flags u32 | entry_crc u32
data_size u32 | header_crc u32 | created_ms u64
data[data_size] | data_crc u32
```

CRC-32 为 IEEE 802.3（与 `zlib.crc32` 同多项式）。`header_crc` 覆盖版本头、全部条目、
`data_size` 与 `created_ms`；`entry_crc`/`data_crc` 覆盖各自字节区。

## 两阶段读取（ap_readx）

1. **按版本号选解析分支**：先读 magic 与 version，再分派到 v1/v2 解析器；未知版本直接拒绝。
2. **补齐旧字段成新结构**：仅 v1 且置 `AP_READ_MIGRATE` 时执行——
   - 每条目补 `flags=0` 与按数据区实算的 `crc`（2 字段/条目）；
   - 头部补 `header_crc=0`（旧包无头部校验，无从重建）与 `created_ms=0`（时间未知）；
   - 补齐总数 = `2 + 2*count`，记录在 `ApIndex.backfilled`，`migrated=1`。

## 迁移开关与拒绝路径

- `ap_read()` / `ap_readx(..., options=0, ...)`：**迁移关闭**。v1 旧包维持历史行为，
  返回 `AP_ERR_VERSION(-4)`，与升级前完全一致。
- `ap_readx(..., AP_READ_MIGRATE, ...)`：v1 走解析 + 补齐，产出与 v2 同构的 `ApIndex`。
- v2 包不受开关影响，始终原生解析。

## 错误报告

所有失败通过 `ApError` 返回 `code`、从包中读到的 `version`（读不到为 0）与人类可读明细：

| 码 | 含义 |
|---|---|
| -1 AP_ERR_PARAM | 参数非法 / OOM |
| -2 AP_ERR_OPEN | 文件打不开 |
| -3 AP_ERR_TRUNC | magic 错、长度不足、字段缺失、数据区截断、条目越界 |
| -4 AP_ERR_VERSION | 未知版本，或迁移关闭时遇到 v1 |
| -5 AP_ERR_NOTFOUND | 条目不存在 |
| -6 AP_ERR_CRC | v2 校验和不匹配 |

每条明细都带 `v<版本号>` 前缀与具体字段/偏移，例如
`v2 package truncated: field header_crc needs 4 bytes at offset 12, package length 14`。

## 构建与核对

```
make test     # 49 项单元测试（含拒绝路径、未知版本、缺字段、截断、坏 CRC）
make demo     # v2 写入/读取 + v1 拒绝与迁移演示
make bench    # v1/v2 各 2000 条目、50 轮计时与逐字段/逐字节误差核对
make verify   # bench + 独立手写解析器交叉核对
```

`tools/hand_parse.py` 是不依赖本库的独立手写解析器（标准库、手工小端解码、自实现 CRC-32），
用 `python3 -I tools/hand_parse.py verify <v1包> <v2包>` 交叉核对两档输出。
