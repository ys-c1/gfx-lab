#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define AP_MAGIC "GPAK"
#define AP_ENTRY_SIZE 40u
#define AP_BLOCK_SIZE 20u
#define AP_MAX_BLOCKS 0x7fffffff

static void put16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

static void put32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static unsigned int get16(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int get32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int read_at(FILE *f, long off, void *dst, size_t n) {
    if (off < 0 || fseek(f, off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, n, f) == n ? 0 : -1;
}

static int file_size(FILE *f, long *size) {
    long cur = ftell(f);
    if (cur < 0 || fseek(f, 0, SEEK_END) != 0) return -1;
    *size = ftell(f);
    if (*size < 0 || fseek(f, cur, SEEK_SET) != 0) return -1;
    return 0;
}

static unsigned long long header_size_v2(int count, int nblocks) {
    return 4ull + 2ull + 2ull + (unsigned long long)count * AP_ENTRY_SIZE + 4ull + 4ull +
           (unsigned long long)nblocks * AP_BLOCK_SIZE;
}

static void copy_name(char *dst, const char *src) {
    size_t n = strlen(src);
    if (n > 31) n = 31;
    memcpy(dst, src, n);
    memset(dst + n, 0, 32 - n);
}

/* v1 writer: byte-for-byte the same output as the original implementation. */
int ap_write(const char *path, const ApEntry *entries, int n, const unsigned char *data, unsigned int data_size) {
    if (!path || (!entries && n > 0) || (!data && data_size > 0)) return AP_E_ARG;
    if (n < 0) return AP_E_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return AP_E_IO;
    unsigned short ver = AP_VERSION_LEGACY, cnt = (unsigned short)n;
    fwrite(AP_MAGIC, 1, 4, f);
    fwrite(&ver, 2, 1, f);
    fwrite(&cnt, 2, 1, f);
    for (int i = 0; i < n; ++i) {
        char name[32];
        copy_name(name, entries[i].name);
        fwrite(name, 1, 32, f);
        fwrite(&entries[i].size, 4, 1, f);
        fwrite(&entries[i].offset, 4, 1, f);
    }
    fwrite(&data_size, 4, 1, f);
    if (data_size) fwrite(data, 1, data_size, f);
    fclose(f);
    return 0;
}

int ap_read(const char *path, ApIndex *idx) { return ap_read_ex(path, idx, NULL); }

int ap_read_ex(const char *path, ApIndex *idx, int *err_block) {
    if (err_block) *err_block = -1;
    if (!path || !idx) return AP_E_ARG;
    memset(idx, 0, sizeof(*idx));
    FILE *f = fopen(path, "rb");
    if (!f) return AP_E_IO;
    long fsz = 0;
    if (file_size(f, &fsz) != 0) { fclose(f); return AP_E_IO; }
    unsigned char head[8];
    if (read_at(f, 0, head, 8) != 0 || memcmp(head, AP_MAGIC, 4) != 0) { fclose(f); return AP_E_FORMAT; }
    unsigned int ver = get16(head + 4), cnt = get16(head + 6);
    if (ver != AP_VERSION_LEGACY && ver != AP_VERSION_CHUNKED) { fclose(f); return AP_E_VERSION; }
    unsigned long long entries_end = 8ull + (unsigned long long)cnt * AP_ENTRY_SIZE;
    if (entries_end + 4ull > (unsigned long long)fsz) { fclose(f); return AP_E_FORMAT; }
    idx->entries = (ApEntry*)calloc(cnt ? cnt : 1, sizeof(ApEntry));
    if (!idx->entries) { fclose(f); return AP_E_ARG; }
    idx->count = (int)cnt;
    idx->version = ver;
    unsigned char rec[AP_ENTRY_SIZE];
    for (unsigned int i = 0; i < cnt; ++i) {
        if (read_at(f, (long)(8ull + (unsigned long long)i * AP_ENTRY_SIZE), rec, AP_ENTRY_SIZE) != 0) {
            ap_free(idx); fclose(f); return AP_E_FORMAT;
        }
        memcpy(idx->entries[i].name, rec, 32);
        idx->entries[i].name[31] = '\0';
        idx->entries[i].size = get32(rec + 32);
        idx->entries[i].offset = get32(rec + 36);
        if ((unsigned long long)idx->entries[i].offset + idx->entries[i].size > 0xffffffffull) {
            ap_free(idx); fclose(f); return AP_E_FORMAT;
        }
    }
    if (read_at(f, (long)entries_end, rec, 4) != 0) { ap_free(idx); fclose(f); return AP_E_FORMAT; }
    idx->data_size = get32(rec);
    for (int i = 0; i < idx->count; ++i) {
        if ((unsigned long long)idx->entries[i].offset + idx->entries[i].size > idx->data_size) {
            ap_free(idx); fclose(f); return AP_E_FORMAT;
        }
    }
    if (ver == AP_VERSION_LEGACY) {
        if ((unsigned long long)idx->data_size > (unsigned long long)fsz - entries_end - 4ull) {
            ap_free(idx); fclose(f); return AP_E_FORMAT;
        }
        fclose(f);
        return 0;
    }
    unsigned char nb[4];
    if (read_at(f, (long)entries_end + 4, nb, 4) != 0) { ap_free(idx); fclose(f); return AP_E_FORMAT; }
    unsigned int nblocks = get32(nb);
    if (nblocks > (unsigned int)AP_MAX_BLOCKS) { ap_free(idx); fclose(f); return AP_E_FORMAT; }
    unsigned long long table_end = entries_end + 8ull + (unsigned long long)nblocks * AP_BLOCK_SIZE;
    if (table_end > (unsigned long long)fsz) { ap_free(idx); fclose(f); return AP_E_FORMAT; }
    idx->blocks = (ApBlock*)calloc(nblocks ? nblocks : 1, sizeof(ApBlock));
    if (!idx->blocks) { ap_free(idx); fclose(f); return AP_E_ARG; }
    idx->block_count = (int)nblocks;
    unsigned char brec[AP_BLOCK_SIZE];
    for (unsigned int i = 0; i < nblocks; ++i) {
        if (read_at(f, (long)(entries_end + 8ull + (unsigned long long)i * AP_BLOCK_SIZE), brec, AP_BLOCK_SIZE) != 0) {
            ap_free(idx); fclose(f); return AP_E_FORMAT;
        }
        ApBlock *b = &idx->blocks[i];
        b->raw_offset = get32(brec);
        b->raw_size = get32(brec + 4);
        b->stored_size = get32(brec + 8);
        b->file_offset = get32(brec + 12);
        b->flags = get32(brec + 16);
        if (b->raw_size == 0) {
            if (err_block) *err_block = (int)i;
            ap_free(idx); fclose(f); return AP_E_BLOCK_EMPTY;
        }
        if (b->flags & AP_BLOCK_COMPRESSED) {
            if (b->stored_size >= b->raw_size) {
                if (err_block) *err_block = (int)i;
                ap_free(idx); fclose(f); return AP_E_BLOCK_GROW;
            }
        } else if (b->stored_size != b->raw_size) {
            if (err_block) *err_block = (int)i;
            ap_free(idx); fclose(f); return AP_E_BLOCK_RANGE;
        }
        if ((unsigned long long)b->raw_offset + b->raw_size > idx->data_size ||
            b->file_offset < table_end ||
            (unsigned long long)b->file_offset + b->stored_size > (unsigned long long)fsz) {
            if (err_block) *err_block = (int)i;
            ap_free(idx); fclose(f); return AP_E_BLOCK_RANGE;
        }
    }
    fclose(f);
    return 0;
}

/* Inflate every block overlapping [offset, offset+size) into dst. */
static int extract_block_range(FILE *f, const ApIndex *idx, unsigned int offset, unsigned int size,
                               unsigned char *dst, long fsz) {
    unsigned long long end = (unsigned long long)offset + size;
    unsigned char *stored = NULL, *raw = NULL;
    for (int i = 0; i < idx->block_count; ++i) {
        const ApBlock *b = &idx->blocks[i];
        unsigned long long b_end = (unsigned long long)b->raw_offset + b->raw_size;
        if (b_end <= offset || b->raw_offset >= end) continue;
        if ((unsigned long long)b->file_offset + b->stored_size > (unsigned long long)fsz) {
            fprintf(stderr, "asset: block %d lies outside the file\n", i);
            return AP_E_BLOCK_RANGE;
        }
        stored = (unsigned char*)malloc(b->stored_size ? b->stored_size : 1);
        if (!stored) return AP_E_ARG;
        if (read_at(f, (long)b->file_offset, stored, b->stored_size) != 0) {
            free(stored);
            fprintf(stderr, "asset: block %d could not be read\n", i);
            return AP_E_IO;
        }
        const unsigned char *src = stored;
        if (b->flags & AP_BLOCK_COMPRESSED) {
            raw = (unsigned char*)malloc(b->raw_size ? b->raw_size : 1);
            if (!raw) { free(stored); return AP_E_ARG; }
            uLongf out_len = b->raw_size;
            if (uncompress(raw, &out_len, stored, b->stored_size) != Z_OK || out_len != b->raw_size) {
                free(stored);
                free(raw);
                fprintf(stderr, "asset: block %d failed to inflate\n", i);
                return AP_E_BLOCK_ZLIB;
            }
            src = raw;
        }
        unsigned long long lo = b->raw_offset > offset ? b->raw_offset : offset;
        unsigned long long hi = b_end < end ? b_end : end;
        memcpy(dst + (lo - offset), src + (lo - b->raw_offset), (size_t)(hi - lo));
        free(stored);
        free(raw);
        stored = NULL;
        raw = NULL;
    }
    return 0;
}

int ap_extract(const char *path, const char *name, unsigned char **out, unsigned int *out_size) {
    if (!path || !name || !out || !out_size) return AP_E_ARG;
    ApIndex idx;
    int rc = ap_read(path, &idx);
    if (rc != 0) return rc;
    int found = -1;
    for (int i = 0; i < idx.count; ++i)
        if (strncmp(idx.entries[i].name, name, 31) == 0) { found = i; break; }
    if (found < 0) { ap_free(&idx); return AP_E_NOTFOUND; }
    FILE *f = fopen(path, "rb");
    if (!f) { ap_free(&idx); return AP_E_IO; }
    long fsz = 0;
    if (file_size(f, &fsz) != 0) { ap_free(&idx); fclose(f); return AP_E_IO; }
    *out_size = idx.entries[found].size;
    *out = (unsigned char*)calloc(*out_size ? *out_size : 1, 1);
    if (!*out) { ap_free(&idx); fclose(f); return AP_E_ARG; }
    if (idx.version == AP_VERSION_LEGACY) {
        unsigned long long header = 8ull + (unsigned long long)idx.count * AP_ENTRY_SIZE + 4ull;
        if (read_at(f, (long)(header + idx.entries[found].offset), *out, *out_size) != 0) {
            free(*out); *out = NULL; ap_free(&idx); fclose(f); return AP_E_FORMAT;
        }
    } else {
        rc = extract_block_range(f, &idx, idx.entries[found].offset, *out_size, *out, fsz);
        if (rc != 0) { free(*out); *out = NULL; ap_free(&idx); fclose(f); return rc; }
    }
    fclose(f);
    ap_free(&idx);
    return 0;
}

/* Validate a block table.  Block-specific failures set *err_block. */
static int validate_blocks(const ApBlock *blocks, int nblocks, unsigned int data_size,
                           unsigned long long table_end, unsigned long long file_end,
                           int *err_block) {
    for (int i = 0; i < nblocks; ++i) {
        const ApBlock *b = &blocks[i];
        if (b->raw_size == 0) { if (err_block) *err_block = i; return AP_E_BLOCK_EMPTY; }
        if (b->flags & AP_BLOCK_COMPRESSED) {
            if (b->stored_size >= b->raw_size) { if (err_block) *err_block = i; return AP_E_BLOCK_GROW; }
        } else if (b->stored_size != b->raw_size) {
            if (err_block) { *err_block = i; }
            return AP_E_BLOCK_RANGE;
        }
        if ((unsigned long long)b->raw_offset + b->raw_size > data_size ||
            b->file_offset < table_end ||
            (unsigned long long)b->file_offset + b->stored_size > file_end) {
            if (err_block) { *err_block = i; }
            return AP_E_BLOCK_RANGE;
        }
    }
    return 0;
}

/* Write header + block payloads.  Each block is written at blocks[i].file_offset. */
static int write_v2(const char *path, const ApEntry *entries, int n,
                    const ApBlock *blocks, int nblocks, unsigned int data_size,
                    unsigned char *const *bufs, const unsigned int *lens, int *err_block) {
    unsigned long long header = header_size_v2(n, nblocks);
    unsigned long long end = header;
    for (int i = 0; i < nblocks; ++i) {
        if ((unsigned long long)blocks[i].file_offset < header) {
            if (err_block) *err_block = i;
            return AP_E_BLOCK_RANGE;
        }
        if ((unsigned long long)blocks[i].file_offset + lens[i] > end)
            end = (unsigned long long)blocks[i].file_offset + lens[i];
    }
    if (end > 0xffffffffull) return AP_E_ARG;
    int rc = validate_blocks(blocks, nblocks, data_size, header, end, err_block);
    if (rc != 0) return rc;
    FILE *f = fopen(path, "wb");
    if (!f) return AP_E_IO;
    unsigned char buf[8];
    memcpy(buf, AP_MAGIC, 4);
    put16(buf + 4, AP_VERSION_CHUNKED);
    put16(buf + 6, (unsigned int)n);
    int bad = fwrite(buf, 1, 8, f) != 8;
    unsigned char rec[AP_ENTRY_SIZE];
    for (int i = 0; !bad && i < n; ++i) {
        memset(rec, 0, sizeof(rec));
        strncpy((char*)rec, entries[i].name, 31);
        rec[31] = '\0';
        put32(rec + 32, entries[i].size);
        put32(rec + 36, entries[i].offset);
        bad = fwrite(rec, 1, AP_ENTRY_SIZE, f) != AP_ENTRY_SIZE;
    }
    unsigned char tail[8];
    put32(tail, data_size);
    put32(tail + 4, (unsigned int)nblocks);
    if (!bad) bad = fwrite(tail, 1, 8, f) != 8;
    unsigned char brec[AP_BLOCK_SIZE];
    for (int i = 0; !bad && i < nblocks; ++i) {
        put32(brec, blocks[i].raw_offset);
        put32(brec + 4, blocks[i].raw_size);
        put32(brec + 8, blocks[i].stored_size);
        put32(brec + 12, blocks[i].file_offset);
        put32(brec + 16, blocks[i].flags);
        bad = fwrite(brec, 1, AP_BLOCK_SIZE, f) != AP_BLOCK_SIZE;
    }
    if (!bad) {
        for (int i = 0; !bad && i < nblocks; ++i) {
            if (fseek(f, (long)blocks[i].file_offset, SEEK_SET) != 0) { bad = 1; break; }
            if (lens[i]) bad = fwrite(bufs[i], 1, lens[i], f) != lens[i];
        }
    }
    if (fclose(f) != 0) bad = 1;
    return bad ? AP_E_IO : 0;
}

int ap_write_chunked(const char *path, const ApEntry *entries, int n,
                     const ApBlock *blocks, int nblocks, unsigned int data_size,
                     const unsigned char *payload, unsigned int payload_size,
                     int *err_block) {
    if (err_block) *err_block = -1;
    if (!path || n < 0 || nblocks < 0 || (!entries && n > 0) || (!blocks && nblocks > 0)) return AP_E_ARG;
    if (n > 0xffff) return AP_E_ARG;
    if (!payload && payload_size > 0) return AP_E_ARG;
    unsigned long long header = header_size_v2(n, nblocks);
    unsigned long long need = 0;
    for (int i = 0; i < nblocks; ++i) need += blocks[i].stored_size;
    if (need != payload_size) return AP_E_ARG;
    unsigned char **bufs = (unsigned char**)calloc(nblocks ? (size_t)nblocks : 1, sizeof(unsigned char*));
    unsigned int *lens = (unsigned int*)calloc(nblocks ? (size_t)nblocks : 1, sizeof(unsigned int));
    if (!bufs || !lens) { free(bufs); free(lens); return AP_E_ARG; }
    unsigned long long off = 0;
    for (int i = 0; i < nblocks; ++i) {
        if ((unsigned long long)blocks[i].file_offset != header + off) {
            free(bufs); free(lens);
            if (err_block) *err_block = i;
            return AP_E_BLOCK_RANGE;
        }
        bufs[i] = (unsigned char*)(payload + off);
        lens[i] = blocks[i].stored_size;
        off += blocks[i].stored_size;
    }
    int rc = write_v2(path, entries, n, blocks, nblocks, data_size, bufs, lens, err_block);
    free(bufs);
    free(lens);
    return rc;
}

/* Form blocks over items[order[k]].  Blocks never mix first-screen and
 * non-first-screen items, and never exceed chunk_size unless a single item
 * does.  block_flags[b] receives AP_BLOCK_FIRST_SCREEN as applicable. */
static void build_blocks(const ApItem *items, const int *order, int n, unsigned int chunk_size,
                         unsigned int *raw_offset, unsigned int *raw_size,
                         unsigned int *item_start, unsigned int *block_flags, int *block_count) {
    int nb = 0;
    unsigned long long cur_off = 0, blk_start = 0, blk_size = 0;
    int cur_fs = -1;
    for (int k = 0; k < n; ++k) {
        int i = order[k];
        if (items[i].size == 0) { item_start[i] = 0; continue; }
        unsigned int fs = items[i].first_screen ? 1u : 0u;
        if (blk_size > 0 && ((int)fs != cur_fs || blk_size + items[i].size > chunk_size)) {
            raw_offset[nb] = (unsigned int)blk_start;
            raw_size[nb] = (unsigned int)blk_size;
            block_flags[nb] = cur_fs ? AP_BLOCK_FIRST_SCREEN : 0u;
            ++nb;
            blk_start = cur_off;
            blk_size = 0;
        }
        if (blk_size == 0) cur_fs = (int)fs;
        item_start[i] = (unsigned int)cur_off;
        cur_off += items[i].size;
        blk_size += items[i].size;
    }
    if (blk_size > 0) {
        raw_offset[nb] = (unsigned int)blk_start;
        raw_size[nb] = (unsigned int)blk_size;
        block_flags[nb] = cur_fs ? AP_BLOCK_FIRST_SCREEN : 0u;
        ++nb;
    }
    *block_count = nb;
}

int ap_pack(const char *path, const ApItem *items, int n, unsigned int chunk_size,
            int compress, int *err_block) {
    if (err_block) *err_block = -1;
    if (!path || n < 0 || n > 0xffff || (!items && n > 0)) return AP_E_ARG;
    unsigned long long total = 0;
    for (int i = 0; i < n; ++i) {
        if (items[i].size && !items[i].data) return AP_E_ARG;
        total += items[i].size;
    }
    if (total > 0xffffffffull) return AP_E_ARG;

    if (!compress) {
        /* Legacy path: one flat v1 payload, same bytes as before. */
        ApEntry *entries = (ApEntry*)calloc(n ? (size_t)n : 1, sizeof(ApEntry));
        unsigned char *stream = (unsigned char*)malloc(total ? (size_t)total : 1);
        if (!entries || !stream) { free(entries); free(stream); return AP_E_ARG; }
        unsigned int off = 0;
        for (int i = 0; i < n; ++i) {
            copy_name(entries[i].name, items[i].name);
            entries[i].size = items[i].size;
            entries[i].offset = off;
            if (items[i].size) memcpy(stream + off, items[i].data, items[i].size);
            off += items[i].size;
        }
        int rc = ap_write(path, entries, n, stream, (unsigned int)total);
        free(entries);
        free(stream);
        return rc;
    }

    if (chunk_size == 0) return AP_E_CHUNK_ZERO;

    /* Stable partition: first-screen items first, so their blocks are a
     * contiguous prefix and one read covers the whole first screen. */
    int *order = (int*)malloc((n ? (size_t)n : 1) * sizeof(int));
    unsigned int *item_start = (unsigned int*)calloc(n ? (size_t)n : 1, sizeof(unsigned int));
    unsigned int *raw_off = (unsigned int*)calloc((n ? (size_t)n : 1), sizeof(unsigned int));
    unsigned int *raw_sz = (unsigned int*)calloc((n ? (size_t)n : 1), sizeof(unsigned int));
    unsigned int *blk_flags = (unsigned int*)calloc((n ? (size_t)n : 1), sizeof(unsigned int));
    unsigned char *stream = (unsigned char*)malloc(total ? (size_t)total : 1);
    if (!order || !item_start || !raw_off || !raw_sz || !blk_flags || !stream) {
        free(order); free(item_start); free(raw_off); free(raw_sz); free(blk_flags); free(stream);
        return AP_E_ARG;
    }
    int k = 0;
    for (int i = 0; i < n; ++i) if (items[i].first_screen && items[i].size) order[k++] = i;
    for (int i = 0; i < n; ++i) if (!items[i].first_screen && items[i].size) order[k++] = i;
    for (int i = 0; i < n; ++i) if (items[i].size == 0) order[k++] = i;
    int nb = 0;
    build_blocks(items, order, n, chunk_size, raw_off, raw_sz, item_start, blk_flags, &nb);

    ApEntry *entries = (ApEntry*)calloc(n ? (size_t)n : 1, sizeof(ApEntry));
    ApBlock *blocks = (ApBlock*)calloc(nb ? (size_t)nb : 1, sizeof(ApBlock));
    unsigned char **stored = (unsigned char**)calloc(nb ? (size_t)nb : 1, sizeof(unsigned char*));
    unsigned int *stored_len = (unsigned int*)calloc(nb ? (size_t)nb : 1, sizeof(unsigned int));
    if (!entries || !blocks || !stored || !stored_len) {
        free(entries); free(blocks); free(stored); free(stored_len);
        free(order); free(item_start); free(raw_off); free(raw_sz); free(blk_flags); free(stream);
        return AP_E_ARG;
    }

    unsigned long long off = 0;
    for (int kk = 0; kk < n; ++kk) {
        int i = order[kk];
        copy_name(entries[i].name, items[i].name);
        entries[i].size = items[i].size;
        entries[i].offset = item_start[i];
        if (items[i].size) memcpy(stream + off, items[i].data, items[i].size);
        off += items[i].size;
    }

    int rc = 0;
    unsigned long long header = header_size_v2(n, nb);
    unsigned long long running = header;
    for (int b = 0; b < nb; ++b) {
        unsigned int rs = raw_sz[b];
        uLongf cap = compressBound(rs);
        unsigned char *tmp = (unsigned char*)malloc(cap ? (size_t)cap : 1);
        if (!tmp) { rc = AP_E_ARG; break; }
        uLongf out_len = cap;
        if (compress2(tmp, &out_len, stream + raw_off[b], rs, Z_BEST_COMPRESSION) != Z_OK) {
            free(tmp);
            if (err_block) *err_block = b;
            rc = AP_E_BLOCK_ZLIB;
            break;
        }
        if (out_len >= rs) {
            free(tmp);
            if (err_block) *err_block = b;
            rc = AP_E_BLOCK_GROW;
            break;
        }
        stored[b] = tmp;
        stored_len[b] = (unsigned int)out_len;
        blocks[b].raw_offset = raw_off[b];
        blocks[b].raw_size = rs;
        blocks[b].stored_size = (unsigned int)out_len;
        blocks[b].file_offset = (unsigned int)running;
        blocks[b].flags = blk_flags[b] | AP_BLOCK_COMPRESSED;
        running += out_len;
    }
    if (rc == 0) rc = write_v2(path, entries, n, blocks, nb, (unsigned int)total, stored, stored_len, err_block);
    for (int b = 0; b < nb; ++b) free(stored[b]);
    free(entries); free(blocks); free(stored); free(stored_len);
    free(order); free(item_start); free(raw_off); free(raw_sz); free(blk_flags); free(stream);
    return rc;
}

static int load_blocks(const char *path, const ApIndex *idx, unsigned char **out,
                       unsigned int *out_size, int *blocks_loaded, int first_screen_only) {
    if (!path || !idx || !out || !out_size) return AP_E_ARG;
    *out = NULL;
    *out_size = 0;
    if (blocks_loaded) *blocks_loaded = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return AP_E_IO;
    long fsz = 0;
    if (file_size(f, &fsz) != 0) { fclose(f); return AP_E_IO; }
    if (idx->version == AP_VERSION_LEGACY) {
        unsigned long long header = 8ull + (unsigned long long)idx->count * AP_ENTRY_SIZE + 4ull;
        unsigned char *buf = (unsigned char*)malloc(idx->data_size ? idx->data_size : 1);
        if (!buf) { fclose(f); return AP_E_ARG; }
        if (read_at(f, (long)header, buf, idx->data_size) != 0) { free(buf); fclose(f); return AP_E_FORMAT; }
        *out = buf;
        *out_size = idx->data_size;
        fclose(f);
        return 0;
    }
    unsigned int highest = 0;
    for (int i = 0; i < idx->block_count; ++i) {
        if (first_screen_only && !(idx->blocks[i].flags & AP_BLOCK_FIRST_SCREEN)) continue;
        unsigned int end = idx->blocks[i].raw_offset + idx->blocks[i].raw_size;
        if (end > highest) highest = end;
    }
    unsigned char *buf = (unsigned char*)calloc(highest ? highest : 1, 1);
    if (!buf) { fclose(f); return AP_E_ARG; }
    int loaded = 0, rc = 0;
    for (int i = 0; i < idx->block_count; ++i) {
        const ApBlock *b = &idx->blocks[i];
        if (first_screen_only && !(b->flags & AP_BLOCK_FIRST_SCREEN)) continue;
        if ((unsigned long long)b->file_offset + b->stored_size > (unsigned long long)fsz) {
            fprintf(stderr, "asset: block %d lies outside the file\n", i);
            rc = AP_E_BLOCK_RANGE;
            break;
        }
        unsigned char *stored = (unsigned char*)malloc(b->stored_size ? b->stored_size : 1);
        if (!stored) { rc = AP_E_ARG; break; }
        if (read_at(f, (long)b->file_offset, stored, b->stored_size) != 0) {
            free(stored); rc = AP_E_IO; break;
        }
        if (b->flags & AP_BLOCK_COMPRESSED) {
            uLongf out_len = b->raw_size;
            if (uncompress(buf + b->raw_offset, &out_len, stored, b->stored_size) != Z_OK ||
                out_len != b->raw_size) {
                free(stored);
                fprintf(stderr, "asset: block %d failed to inflate\n", i);
                rc = AP_E_BLOCK_ZLIB;
                break;
            }
        } else {
            memcpy(buf + b->raw_offset, stored, b->raw_size);
        }
        free(stored);
        ++loaded;
    }
    fclose(f);
    if (rc != 0) { free(buf); return rc; }
    *out = buf;
    *out_size = highest;
    if (blocks_loaded) *blocks_loaded = loaded;
    return 0;
}

int ap_load_first_screen(const char *path, const ApIndex *idx, unsigned char **out,
                         unsigned int *out_size, int *blocks_loaded) {
    return load_blocks(path, idx, out, out_size, blocks_loaded, 1);
}

int ap_load_all(const char *path, const ApIndex *idx, unsigned char **out,
                unsigned int *out_size, int *blocks_loaded) {
    return load_blocks(path, idx, out, out_size, blocks_loaded, 0);
}

void ap_free(ApIndex *idx) {
    if (idx) {
        free(idx->entries);
        free(idx->blocks);
        idx->entries = NULL;
        idx->blocks = NULL;
        idx->count = 0;
        idx->block_count = 0;
    }
}
