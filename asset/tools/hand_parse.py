#!/usr/bin/env python3
"""独立手写 GPAK 解析器（不依赖 asset 库），用于交叉核对 C 实现。

用法：
  python3 -I hand_parse.py parse <file.gpak> [--migrate]   # 手写解析并打印结构
  python3 -I hand_parse.py verify <v1.gpak> <v2.gpak>      # 两档对比核对
所有多字节整数按小端手工解码，CRC-32 自行实现（不调用 zlib）。
"""
import sys
import struct
import time

MAGIC = b"GPAK"
V1, V2 = 1, 2


class PakError(Exception):
    def __init__(self, code, version, msg):
        super().__init__(msg)
        self.code = code
        self.version = version


def u16(b, at):
    return b[at] | (b[at + 1] << 8)


def u32(b, at):
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)


def u64(b, at):
    v = 0
    for i in range(7, -1, -1):
        v = (v << 8) | b[at + i]
    return v


# ---- 手写 CRC-32（IEEE 802.3，与 zlib.crc32 同多项式，独立建表） ----
def _build_table():
    table = []
    for n in range(256):
        c = n
        for _ in range(8):
            c = (0xEDB88320 ^ (c >> 1)) if (c & 1) else (c >> 1)
        table.append(c)
    return table


_TABLE = _build_table()


def crc32(buf):
    c = 0xFFFFFFFF
    for x in buf:
        c = _TABLE[(c ^ x) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def _need(length, at, need, version, what):
    if at > length or need > length - at:
        raise PakError(-3, version,
                       f"v{version} truncated: field {what} needs {need} at offset {at}, length {length}")


def hand_parse(raw, migrate=False):
    """返回 (index_dict, backfilled_count)。完全按格式契约手工拆字节。"""
    length = len(raw)
    if length < 4 or raw[:4] != MAGIC:
        raise PakError(-3, 0, f"bad magic, length {length}")
    if length < 8:
        raise PakError(-3, 0, f"header too short, length {length}")
    ver = u16(raw, 4)
    cnt = u16(raw, 6)

    if ver not in (V1, V2):
        raise PakError(-4, ver, f"unknown version {ver}")
    if ver == V1 and not migrate:
        raise PakError(-4, ver, f"v{ver} rejected: migration disabled")

    at = 8
    entries = []
    stride = 40 if ver == V1 else 48
    for i in range(cnt):
        _need(length, at, 32, ver, "entry.name")
        name = raw[at:at + 32].split(b"\x00", 1)[0].decode("ascii", "replace")
        at += 32
        _need(length, at, 4, ver, "entry.size")
        size = u32(raw, at); at += 4
        _need(length, at, 4, ver, "entry.offset")
        offset = u32(raw, at); at += 4
        if ver == V2:
            _need(length, at, 4, ver, "entry.flags")
            flags = u32(raw, at); at += 4
            _need(length, at, 4, ver, "entry.crc")
            crc = u32(raw, at); at += 4
        else:
            flags, crc = None, None   # 待阶段二补齐
        entries.append({"name": name, "size": size, "offset": offset,
                        "flags": flags, "crc": crc})

    _need(length, at, 4, ver, "data_size")
    data_size = u32(raw, at); at += 4
    if ver == V2:
        _need(length, at, 4, ver, "header_crc"); header_crc = u32(raw, at); at += 4
        _need(length, at, 8, ver, "created_ms"); created_ms = u64(raw, at); at += 8
        data_at = at
    else:
        data_at = at
        header_crc, created_ms = None, None

    if length - data_at < data_size:
        raise PakError(-3, ver, f"v{ver} data region truncated")
    payload = raw[data_at:data_at + data_size]

    index = {"version": ver, "count": cnt, "data_size": data_size,
             "entries": entries, "data_at": data_at, "payload": payload}

    backfilled = 0
    if ver == V1:
        # 阶段二：把旧字段补齐成 v2 结构
        for e in entries:
            e["flags"] = 0
            seg = payload[e["offset"]:e["offset"] + e["size"]]
            if len(seg) != e["size"]:
                raise PakError(-3, ver, f"v{ver} entry {e['name']} payload truncated")
            e["crc"] = crc32(seg)
            backfilled += 2
        header_crc, created_ms = 0, 0
        backfilled += 2
        index["migrated"] = True
    else:
        index["migrated"] = False
        # v2 整体数据校验
        if length - data_at < data_size + 4:
            raise PakError(-3, ver, "v2 missing data_crc")
        stored = u32(raw, data_at + data_size)
        if stored != crc32(payload):
            raise PakError(-6, ver, "v2 data_crc mismatch")
    index["header_crc"] = header_crc
    index["created_ms"] = created_ms
    index["backfilled"] = backfilled
    return index


def cmd_parse(path, migrate):
    with open(path, "rb") as f:
        raw = f.read()
    t0 = time.perf_counter()
    idx = hand_parse(raw, migrate=migrate)
    dt = (time.perf_counter() - t0) * 1000.0
    print(f"file={path} version={idx['version']} count={idx['count']} "
          f"data_size={idx['data_size']} migrated={idx['migrated']} "
          f"backfilled={idx['backfilled']} parse_ms={dt:.3f}")
    for e in idx["entries"][:3] + (idx["entries"][-2:] if idx["count"] > 5 else []):
        print(f"  {e['name']:<14} size={e['size']:<6} off={e['offset']:<8} "
              f"flags={e['flags']} crc={e['crc']:08x}")
    if idx["count"] > 5:
        print(f"  ... ({idx['count'] - 5} more)")
    print(f"  header_crc={idx['header_crc']} created_ms={idx['created_ms']}")


def cmd_verify(v1_path, v2_path):
    with open(v1_path, "rb") as f:
        v1_raw = f.read()
    with open(v2_path, "rb") as f:
        v2_raw = f.read()
    v1 = hand_parse(v1_raw, migrate=True)
    v2 = hand_parse(v2_raw, migrate=False)

    errors = []
    if v1["count"] != v2["count"]:
        errors.append(f"count {v1['count']} != {v2['count']}")
    if v1["data_size"] != v2["data_size"]:
        errors.append("data_size differs")
    for a, b in zip(v1["entries"], v2["entries"]):
        for key in ("name", "size", "offset", "crc"):
            if a[key] != b[key]:
                errors.append(f"entry {a['name']} field {key}: {a[key]} != {b[key]}")
    if v1["payload"] != v2["payload"]:
        errors.append("payload bytes differ")
    if v1["backfilled"] != 2 + 2 * v1["count"]:
        errors.append(f"backfilled count {v1['backfilled']} unexpected")
    if v2["backfilled"] != 0:
        errors.append("native v2 should backfill zero fields")

    print(f"v1: version={v1['version']} migrated={v1['migrated']} backfilled={v1['backfilled']}")
    print(f"v2: version={v2['version']} migrated={v2['migrated']} backfilled={v2['backfilled']}")
    print(f"entries={v1['count']} data_bytes={v1['data_size']}")
    print("hand-parser cross-check:", "PASS" if not errors else "FAIL")
    for m in errors:
        print("  ERROR:", m)
    return 1 if errors else 0


def main(argv):
    if len(argv) >= 2 and argv[0] == "parse":
        migrate = "--migrate" in argv
        cmd_parse(argv[1], migrate)
        return 0
    if len(argv) == 3 and argv[0] == "verify":
        return cmd_verify(argv[1], argv[2])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
