#define _POSIX_C_SOURCE 200809L
#include "asset.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- 小端定长读写：格式契约显式化，不依赖机器字节序与结构对齐 ---------- */

static void wr_u16(FILE *f, unsigned int v) {
    unsigned char b[2] = {v & 0xff, (v >> 8) & 0xff};
    fwrite(b, 1, 2, f);
}
static void wr_u32(FILE *f, unsigned int v) {
    unsigned char b[4] = {v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff};
    fwrite(b, 1, 4, f);
}
static void wr_u64(FILE *f, unsigned long long v) {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = (unsigned char)((v >> (8 * i)) & 0xff);
    fwrite(b, 1, 8, f);
}
static unsigned int rd_u16(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static unsigned int rd_u32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static unsigned long long rd_u64(const unsigned char *p) {
    unsigned long long v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

/* ---------- IEEE CRC-32（与 Python zlib.crc32 同一多项式/初值/异或输出） ---------- */

static unsigned int crc_table[256];
static int crc_table_ready = 0;

static void crc32_init(void) {
    for (unsigned int i = 0; i < 256; ++i) {
        unsigned int c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_table_ready = 1;
}

unsigned int ap_crc32(const unsigned char *buf, size_t len) {
    if (!crc_table_ready) crc32_init();
    unsigned int c = 0xffffffffu;
    for (size_t i = 0; i < len; ++i)
        c = crc_table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static unsigned long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (unsigned long)ts.tv_sec * 1000ul + (unsigned long)(ts.tv_nsec / 1000000);
}

static void err_set(ApError *e, int code, unsigned int ver, const char *fmt, ...) {
    if (!e) return;
    e->code = code;
    e->version = ver;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->detail, sizeof(e->detail), fmt, ap);
    va_end(ap);
}

/* ---------- 整文件载入，长度不足一律在解析处显式拦截 ---------- */

static int load_file(const char *path, unsigned char **out_buf, size_t *out_len, ApError *err) {
    FILE *f = fopen(path, "rb");
    if (!f) { err_set(err, AP_ERR_OPEN, 0, "cannot open file: %s", path); return AP_ERR_OPEN; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); err_set(err, AP_ERR_TRUNC, 0, "seek failed"); return AP_ERR_TRUNC; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); err_set(err, AP_ERR_TRUNC, 0, "ftell failed"); return AP_ERR_TRUNC; }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); err_set(err, AP_ERR_PARAM, 0, "out of memory"); return AP_ERR_PARAM; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        err_set(err, AP_ERR_TRUNC, 0, "short read: want %ld got %zu", sz, got);
        return AP_ERR_TRUNC;
    }
    *out_buf = buf;
    *out_len = (size_t)sz;
    return AP_OK;
}

/* 边界检查：从 at 起还需要 need 字节 */
static int need(size_t len, size_t at, size_t need, unsigned int ver, ApError *err, const char *what) {
    if (at > len || need > len - at) {
        err_set(err, AP_ERR_TRUNC, ver,
                "v%u package truncated: field %s needs %zu bytes at offset %zu, package length %zu",
                ver, what, need, at, len);
        return AP_ERR_TRUNC;
    }
    return AP_OK;
}

/* ================= 写端：v2（默认）与 v1（造旧包用） ================= */

static int validate_inputs(const ApEntry *entries, int n, const unsigned char *data, unsigned int data_size) {
    if ((!entries && n > 0) || (!data && data_size > 0) || n < 0) return AP_ERR_PARAM;
    for (int i = 0; i < n; ++i) {
        if (strnlen(entries[i].name, 32) >= 32) return AP_ERR_PARAM;
        /* offset+size 溢出与越界检查 */
        if (entries[i].size > data_size || entries[i].offset > data_size - entries[i].size)
            return AP_ERR_PARAM;
    }
    return AP_OK;
}

int ap_write_v1(const char *path, const ApEntry *entries, int n, const unsigned char *data, unsigned int data_size) {
    int rc = validate_inputs(entries, n, data, data_size);
    if (rc != AP_OK) return rc;
    FILE *f = fopen(path, "wb");
    if (!f) return AP_ERR_OPEN;
    unsigned short cnt = (unsigned short)n;
    fwrite(AP_MAGIC, 1, 4, f);
    wr_u16(f, AP_VERSION_1);
    wr_u16(f, cnt);
    for (int i = 0; i < n; ++i) {
        char name[32];
        memset(name, 0, sizeof(name));
        strncpy(name, entries[i].name, sizeof(name) - 1);
        fwrite(name, 1, 32, f);
        wr_u32(f, entries[i].size);
        wr_u32(f, entries[i].offset);
    }
    wr_u32(f, data_size);
    if (data_size) fwrite(data, 1, data_size, f);
    fclose(f);
    return AP_OK;
}

int ap_write(const char *path, const ApEntry *entries, int n, const unsigned char *data, unsigned int data_size) {
    int rc = validate_inputs(entries, n, data, data_size);
    if (rc != AP_OK) return rc;
    if (!crc_table_ready) crc32_init();
    FILE *f = fopen(path, "wb");
    if (!f) return AP_ERR_OPEN;

    unsigned int cnt = (unsigned int)n;
    unsigned long created = now_ms();

    /* 先算每个条目的 payload crc */
    unsigned int *entry_crc = NULL;
    if (n) {
        entry_crc = (unsigned int *)calloc((size_t)n, sizeof(unsigned int));
        if (!entry_crc) { fclose(f); return AP_ERR_PARAM; }
    }
    for (int i = 0; i < n; ++i)
        entry_crc[i] = entries[i].size ? ap_crc32(data + entries[i].offset, entries[i].size)
                                       : ap_crc32(NULL, 0);
    unsigned int data_crc = data_size ? ap_crc32(data, data_size) : ap_crc32(NULL, 0);

    fwrite(AP_MAGIC, 1, 4, f);
    wr_u16(f, AP_VERSION_2);
    wr_u16(f, cnt);
    /* header_crc 的第一段：ver/cnt/全部条目 */
    unsigned int hcrc = 0xffffffffu;
    #define CRC_FEED(p, l) do { \
        for (size_t _i = 0; _i < (size_t)(l); ++_i) hcrc = crc_table[(hcrc ^ (p)[_i]) & 0xff] ^ (hcrc >> 8); \
    } while (0)
    unsigned char hb[8];
    hb[0] = AP_VERSION_2 & 0xff; hb[1] = (AP_VERSION_2 >> 8) & 0xff;
    hb[2] = cnt & 0xff;          hb[3] = (cnt >> 8) & 0xff;
    CRC_FEED(hb, 4);

    for (unsigned int i = 0; i < cnt; ++i) {
        char name[32];
        memset(name, 0, sizeof(name));
        strncpy(name, entries[i].name, sizeof(name) - 1);
        fwrite(name, 1, 32, f);
        wr_u32(f, entries[i].size);
        wr_u32(f, entries[i].offset);
        wr_u32(f, entries[i].flags);
        wr_u32(f, entry_crc[i]);
        CRC_FEED((const unsigned char *)name, 32);
        unsigned char eb[16];
        for (int k = 0; k < 4; ++k) {
            unsigned int vv = (k == 0) ? entries[i].size : (k == 1) ? entries[i].offset :
                              (k == 2) ? entries[i].flags : entry_crc[i];
            eb[k * 4 + 0] = vv & 0xff; eb[k * 4 + 1] = (vv >> 8) & 0xff;
            eb[k * 4 + 2] = (vv >> 16) & 0xff; eb[k * 4 + 3] = (vv >> 24) & 0xff;
        }
        CRC_FEED(eb, 16);
    }
    wr_u32(f, data_size);
    wr_u32(f, 0);                       /* header_crc 占位 */
    wr_u64(f, created);
    /* header_crc 的第二段：data_size 与 created_ms（占位字段本身不计入） */
    for (int k = 0; k < 4; ++k) hb[k] = (data_size >> (8 * k)) & 0xff;
    CRC_FEED(hb, 4);
    unsigned long long cms = created;
    unsigned char cb[8];
    for (int k = 0; k < 8; ++k) cb[k] = (unsigned char)((cms >> (8 * k)) & 0xff);
    CRC_FEED(cb, 8);
    hcrc ^= 0xffffffffu;

    /* 回填 header_crc */
    long crc_pos = 8 + (long)cnt * 48 + 4;
    fseek(f, crc_pos, SEEK_SET);
    wr_u32(f, hcrc);
    fseek(f, 0, SEEK_END);

    if (data_size) fwrite(data, 1, data_size, f);
    wr_u32(f, data_crc);

    free(entry_crc);
    fclose(f);
    return AP_OK;
    #undef CRC_FEED
}

/* ================= 读端：阶段一按版本号分派解析分支 ================= */

static int parse_v1(const unsigned char *buf, size_t len, unsigned int ver, unsigned int cnt,
                    ApIndex *idx, ApError *err) {
    size_t at = 8;
    idx->entries = (ApEntry *)calloc(cnt ? cnt : 1, sizeof(ApEntry));
    if (!idx->entries) { err_set(err, AP_ERR_PARAM, ver, "out of memory"); return AP_ERR_PARAM; }
    idx->count = (int)cnt;
    for (unsigned int i = 0; i < cnt; ++i) {
        if (need(len, at, 32, ver, err, "entry.name") != AP_OK) return AP_ERR_TRUNC;
        memcpy(idx->entries[i].name, buf + at, 32);
        idx->entries[i].name[31] = '\0';
        at += 32;
        if (need(len, at, 4, ver, err, "entry.size") != AP_OK) return AP_ERR_TRUNC;
        idx->entries[i].size = rd_u32(buf + at); at += 4;
        if (need(len, at, 4, ver, err, "entry.offset") != AP_OK) return AP_ERR_TRUNC;
        idx->entries[i].offset = rd_u32(buf + at); at += 4;
    }
    if (need(len, at, 4, ver, err, "data_size") != AP_OK) return AP_ERR_TRUNC;
    idx->data_size = rd_u32(buf + at); at += 4;

    /* 数据区长度与条目越界检查 */
    size_t data_at = at;
    if (len - data_at < (size_t)idx->data_size) {
        err_set(err, AP_ERR_TRUNC, ver,
                "v%u package truncated: data region needs %u bytes, have %zu",
                ver, idx->data_size, len - data_at);
        return AP_ERR_TRUNC;
    }
    for (int i = 0; i < idx->count; ++i) {
        ApEntry *e = &idx->entries[i];
        if (e->size > idx->data_size || e->offset > idx->data_size - e->size) {
            err_set(err, AP_ERR_TRUNC, ver,
                    "v%u entry %d (%s) offset/size out of data region", ver, i, e->name);
            return AP_ERR_TRUNC;
        }
    }
    idx->header_crc = 0;       /* v1 无此字段，迁移前保持未补齐 */
    idx->created_ms = 0;
    idx->version = ver;
    return AP_OK;
}

static int parse_v2(const unsigned char *buf, size_t len, unsigned int ver, unsigned int cnt,
                    ApIndex *idx, ApError *err) {
    if (!crc_table_ready) crc32_init();
    size_t at = 8;
    idx->entries = (ApEntry *)calloc(cnt ? cnt : 1, sizeof(ApEntry));
    if (!idx->entries) { err_set(err, AP_ERR_PARAM, ver, "out of memory"); return AP_ERR_PARAM; }
    idx->count = (int)cnt;
    for (unsigned int i = 0; i < cnt; ++i) {
        if (need(len, at, 32, ver, err, "entry.name") != AP_OK) return AP_ERR_TRUNC;
        memcpy(idx->entries[i].name, buf + at, 32);
        idx->entries[i].name[31] = '\0';
        at += 32;
        if (need(len, at, 4, ver, err, "entry.size") != AP_OK) return AP_ERR_TRUNC;
        idx->entries[i].size = rd_u32(buf + at); at += 4;
        if (need(len, at, 4, ver, err, "entry.offset") != AP_OK) return AP_ERR_TRUNC;
        idx->entries[i].offset = rd_u32(buf + at); at += 4;
        if (need(len, at, 4, ver, err, "entry.flags") != AP_OK) return AP_ERR_TRUNC;
        idx->entries[i].flags = rd_u32(buf + at); at += 4;
        if (need(len, at, 4, ver, err, "entry.crc") != AP_OK) return AP_ERR_TRUNC;
        idx->entries[i].crc = rd_u32(buf + at); at += 4;
    }
    if (need(len, at, 4, ver, err, "data_size") != AP_OK) return AP_ERR_TRUNC;
    idx->data_size = rd_u32(buf + at); at += 4;
    if (need(len, at, 4, ver, err, "header_crc") != AP_OK) return AP_ERR_TRUNC;
    idx->header_crc = rd_u32(buf + at); at += 4;
    if (need(len, at, 8, ver, err, "created_ms") != AP_OK) return AP_ERR_TRUNC;
    idx->created_ms = (unsigned long)rd_u64(buf + at); at += 8;

    size_t data_at = at;
    if (need(len, data_at, (size_t)idx->data_size + 4, ver, err, "data region + data_crc") != AP_OK)
        return AP_ERR_TRUNC;
    for (int i = 0; i < idx->count; ++i) {
        ApEntry *e = &idx->entries[i];
        if (e->size > idx->data_size || e->offset > idx->data_size - e->size) {
            err_set(err, AP_ERR_TRUNC, ver,
                    "v%u entry %d (%s) offset/size out of data region", ver, i, e->name);
            return AP_ERR_TRUNC;
        }
    }

    /* 数据区整体校验 */
    unsigned int want_crc = rd_u32(buf + data_at + idx->data_size);
    unsigned int got_crc = idx->data_size ? ap_crc32(buf + data_at, idx->data_size) : ap_crc32(NULL, 0);
    if (want_crc != got_crc) {
        err_set(err, AP_ERR_CRC, ver,
                "v%u data_crc mismatch: stored %08x computed %08x", ver, want_crc, got_crc);
        return AP_ERR_CRC;
    }
    idx->version = ver;
    idx->migrated = 0;
    idx->backfilled = 0;
    return AP_OK;
}

/* ================= 阶段二：把 v1 旧字段补齐成 v2 结构 ================= */

static int migrate_v1_to_v2(const unsigned char *buf, size_t len, ApIndex *idx, ApError *err) {
    if (!crc_table_ready) crc32_init();
    unsigned int ver = idx->version;          /* 保留包内真实版本号 */
    size_t data_at = 8 + (size_t)idx->count * 40 + 4;
    int filled = 0;
    for (int i = 0; i < idx->count; ++i) {
        ApEntry *e = &idx->entries[i];
        e->flags = 0;                                            /* 旧包无标记，补默认值 */
        if (len - data_at < (size_t)e->offset + e->size) {
            err_set(err, AP_ERR_TRUNC, ver,
                    "v%u migration: entry %s payload beyond package length", ver, e->name);
            return AP_ERR_TRUNC;
        }
        e->crc = e->size ? ap_crc32(buf + data_at + e->offset, e->size) : ap_crc32(NULL, 0);
        filled += 2;                                            /* flags + crc */
    }
    idx->header_crc = 0;      /* 0 = 旧包本无头部校验，无从重建 */
    idx->created_ms = 0;      /* 0 = 旧包无时间戳，未知 */
    filled += 2;              /* header_crc + created_ms */
    idx->migrated = 1;
    idx->backfilled = filled; /* 共 2 + 2*count 个字段 */
    return AP_OK;
}

int ap_readx(const char *path, unsigned int options, ApIndex *idx, ApError *err) {
    ApError local;
    if (!err) err = &local;
    memset(err, 0, sizeof(*err));
    if (!path || !idx) { err_set(err, AP_ERR_PARAM, 0, "null path or index"); return AP_ERR_PARAM; }
    memset(idx, 0, sizeof(*idx));

    unsigned char *buf = NULL;
    size_t len = 0;
    int rc = load_file(path, &buf, &len, err);
    if (rc != AP_OK) return rc;

    /* 阶段一：先识别 magic 与版本号，再选解析分支 */
    if (len < 4 || memcmp(buf, AP_MAGIC, 4) != 0) {
        err_set(err, AP_ERR_TRUNC, 0, "bad magic: expected '%s', package length %zu", AP_MAGIC, len);
        rc = AP_ERR_TRUNC;
        goto fail;
    }
    if (len < 8) {
        err_set(err, AP_ERR_TRUNC, 0,
                "package too short for version/count header: length %zu, need 8", len);
        rc = AP_ERR_TRUNC;
        goto fail;
    }
    unsigned int ver = rd_u16(buf + 4);
    unsigned int cnt = rd_u16(buf + 6);

    if (ver == AP_VERSION_1) {
        if (!(options & AP_READ_MIGRATE)) {
            /* 迁移关闭：旧包仍走原先的拒绝路径（历史行为 ver != 当前版本 -> -4） */
            err_set(err, AP_ERR_VERSION, ver,
                    "v%u package rejected: migration disabled (enable AP_READ_MIGRATE to upgrade)", ver);
            rc = AP_ERR_VERSION;
            goto fail;
        }
        rc = parse_v1(buf, len, ver, cnt, idx, err);
        if (rc != AP_OK) goto fail;       /* parse 失败时 entries 可能已部分分配，统一清理 */
        /* 阶段二：补齐为 v2 结构 */
        rc = migrate_v1_to_v2(buf, len, idx, err);
        if (rc != AP_OK) goto fail;
    } else if (ver == AP_VERSION_2) {
        rc = parse_v2(buf, len, ver, cnt, idx, err);
        if (rc != AP_OK) goto fail;
    } else {
        err_set(err, AP_ERR_VERSION, ver,
                "unknown package version %u: supported versions are %u and %u",
                ver, AP_VERSION_1, AP_VERSION_2);
        rc = AP_ERR_VERSION;
        goto fail;
    }
    free(buf);
    return AP_OK;

fail:
    free(idx->entries);
    idx->entries = NULL;
    idx->count = 0;
    free(buf);
    return rc;
}

int ap_read(const char *path, ApIndex *idx) {
    return ap_readx(path, 0, idx, NULL);   /* 默认迁移关闭，保留历史拒绝语义 */
}

/* ================= 条目提取 ================= */

int ap_extractx(const char *path, const char *name, unsigned int options,
                unsigned char **out, unsigned int *out_size, ApError *err) {
    ApError local;
    if (!err) err = &local;
    if (!path || !name || !out || !out_size) {
        err_set(err, AP_ERR_PARAM, 0, "null argument to ap_extractx");
        return AP_ERR_PARAM;
    }
    ApIndex idx;
    int rc = ap_readx(path, options, &idx, err);
    if (rc != AP_OK) return rc;

    int found = -1;
    for (int i = 0; i < idx.count; ++i)
        if (strncmp(idx.entries[i].name, name, 31) == 0) { found = i; break; }
    if (found < 0) {
        err_set(err, AP_ERR_NOTFOUND, idx.version, "v%u entry not found: %s", idx.version, name);
        ap_free(&idx);
        return AP_ERR_NOTFOUND;
    }

    unsigned char *buf = NULL;
    size_t len = 0;
    rc = load_file(path, &buf, &len, err);
    if (rc != AP_OK) { ap_free(&idx); return rc; }

    ApEntry *e = &idx.entries[found];
    size_t data_at = (idx.version == AP_VERSION_1)
                     ? 8 + (size_t)idx.count * 40 + 4
                     : 8 + (size_t)idx.count * 48 + 16;
    if (len - data_at < (size_t)e->offset + e->size) {
        err_set(err, AP_ERR_TRUNC, idx.version,
                "v%u entry %s payload truncated: need %u+%u, have %zu",
                idx.version, name, e->offset, e->size, len - data_at);
        free(buf); ap_free(&idx);
        return AP_ERR_TRUNC;
    }
    *out_size = e->size;
    *out = (unsigned char *)malloc(e->size ? e->size : 1);
    if (!*out) { free(buf); ap_free(&idx); err_set(err, AP_ERR_PARAM, idx.version, "oom"); return AP_ERR_PARAM; }
    if (e->size) memcpy(*out, buf + data_at + e->offset, e->size);

    /* v2 原生包与已迁移 v1 包都具备条目 crc，逐条目核验 */
    unsigned int got = e->size ? ap_crc32(*out, e->size) : ap_crc32(NULL, 0);
    if (got != e->crc) {
        err_set(err, AP_ERR_CRC, idx.version,
                "v%u entry %s crc mismatch: stored %08x computed %08x",
                idx.version, name, e->crc, got);
        free(*out); *out = NULL; free(buf); ap_free(&idx);
        return AP_ERR_CRC;
    }
    free(buf);
    ap_free(&idx);
    return AP_OK;
}

int ap_extract(const char *path, const char *name, unsigned char **out, unsigned int *out_size) {
    return ap_extractx(path, name, 0, out, out_size, NULL);
}

void ap_free(ApIndex *idx) {
    if (idx) {
        free(idx->entries);
        idx->entries = NULL;
        idx->count = 0;
    }
}
