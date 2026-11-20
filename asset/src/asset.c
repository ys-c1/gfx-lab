#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AP_MAGIC "GPAK"
#define AP_VERSION AP_VERSION_LEGACY
#define AP_ENTRY_BYTES 40u
#define AP_LAYOUT_BYTES 12u /* block_count + block_size + copies, u32 each */

/* v2 on-disk layout:
 *
 *   magic | ver=2 | count | entries[count] | data_size
 *   block_count | block_size | copies | checksum[block_count]
 *   data[data_size]
 *   copy[0..copies-1][data_size]
 *
 * Data offsets in the index are relative to the start of `data`.  Putting
 * the layout fields before the data lets a truncated file be attributed to a
 * block without trusting anything inside the missing region.
 */

/* ---------------------------------------------------------------- helpers */

static unsigned int ap_hash(const unsigned char *p, unsigned int n) {
    unsigned int h = 2166136261u; /* FNV-1a */
    for (unsigned int i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void ap_copy_name(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

static int ap_write_u32(FILE *f, unsigned int v) { return fwrite(&v, 4, 1, f) == 1 ? 0 : -1; }
static int ap_write_u16(FILE *f, unsigned short v) { return fwrite(&v, 2, 1, f) == 1 ? 0 : -1; }
static int ap_read_u32(FILE *f, unsigned int *v) { return fread(v, 4, 1, f) == 1 ? 0 : -1; }
static int ap_read_u16(FILE *f, unsigned short *v) { return fread(v, 2, 1, f) == 1 ? 0 : -1; }

static long ap_file_size(FILE *f) {
    long cur = ftell(f);
    if (cur < 0) return -1;
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    long end = ftell(f);
    fseek(f, cur, SEEK_SET);
    return end;
}

/* Size of everything before the data region: magic/ver/count, entries,
 * data_size, and (v2 only) the layout block plus checksum table. */
static unsigned int ap_prefix_size(const ApIndex *idx) {
    unsigned int n = 4 + 2 + 2 + (unsigned int)idx->count * AP_ENTRY_BYTES + 4;
    if (idx->version == AP_VERSION_CHECKED)
        n += AP_LAYOUT_BYTES + idx->block_count * 4;
    return n;
}

static int ap_read_header(FILE *f, ApIndex *idx, unsigned int *hdr) {
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, AP_MAGIC, 4) != 0) return AP_E_FORMAT;
    unsigned short ver = 0, cnt = 0;
    if (ap_read_u16(f, &ver) != 0) return AP_E_FORMAT;
    if (ver != AP_VERSION_LEGACY && ver != AP_VERSION_CHECKED) return AP_E_VERSION;
    if (ap_read_u16(f, &cnt) != 0) return AP_E_FORMAT;
    idx->version = ver;
    idx->block_count = 0;
    idx->entries = (ApEntry*)calloc(cnt ? cnt : 1, sizeof(ApEntry));
    if (!idx->entries) return AP_E_ARG;
    idx->count = cnt;
    for (int i = 0; i < cnt; ++i) {
        if (fread(idx->entries[i].name, 1, 32, f) != 32 ||
            ap_read_u32(f, &idx->entries[i].size) != 0 ||
            ap_read_u32(f, &idx->entries[i].offset) != 0) {
            free(idx->entries); idx->entries = NULL; idx->count = 0;
            return AP_E_FORMAT;
        }
    }
    if (ap_read_u32(f, &idx->data_size) != 0) {
        free(idx->entries); idx->entries = NULL; idx->count = 0;
        return AP_E_FORMAT;
    }
    if (hdr) *hdr = 4 + 2 + 2 + (unsigned int)cnt * AP_ENTRY_BYTES + 4;
    return 0;
}

typedef struct {
    unsigned int block_size, copies, block_count;
    unsigned int data_at;   /* file offset of the data region */
} ApLayout;

/* Parse the v2 layout block and checksum table.  `flt` receives the block
 * that a truncated file cuts through. */
static int ap_read_layout(FILE *f, const ApIndex *idx, unsigned int hdr,
                          long fsize, ApLayout *lay, unsigned int **sums_out,
                          ApFault *flt) {
    unsigned int dsz = idx->data_size;
    unsigned int bc = 0, bs = 0, cp = 0;
    if (dsz == 0) {
        if (flt) { flt->code = AP_E_FORMAT; flt->block = -1; flt->offset = hdr; }
        return AP_E_FORMAT;
    }
    if (fseek(f, (long)hdr, SEEK_SET) != 0) return AP_E_IO;
    if (fread(&bc, 4, 1, f) != 1 || fread(&bs, 4, 1, f) != 1 ||
        fread(&cp, 4, 1, f) != 1) {
        if (flt) { flt->code = AP_E_LENGTH; flt->block = -1; flt->offset = hdr; }
        return AP_E_LENGTH;
    }
    if (bs == 0 || cp == 0 || cp > 64 || bc != (dsz + bs - 1) / bs) {
        if (flt) { flt->code = AP_E_FORMAT; flt->block = -1; flt->offset = hdr; }
        return AP_E_FORMAT;
    }

    long sums_at = (long)hdr + AP_LAYOUT_BYTES;
    long data_at = sums_at + (long)bc * 4;
    if (fsize < data_at) {
        if (flt) { flt->code = AP_E_LENGTH; flt->block = -1; flt->offset = (unsigned int)fsize; }
        return AP_E_LENGTH;
    }
    if (fsize < data_at + (long)dsz) {
        /* Primary data cut short: name the block holding the last byte. */
        unsigned int block = (unsigned int)((fsize - data_at) / bs);
        if (block >= bc) block = bc - 1;
        if (flt) { flt->code = AP_E_LENGTH; flt->block = (int)block;
                   flt->offset = (unsigned int)fsize; }
        return AP_E_LENGTH;
    }
    long copies_at = data_at + (long)dsz;
    long copies_end = copies_at + (long)cp * (long)dsz;
    if (fsize < copies_end) {
        /* Redundant copy cut short: name the block inside that copy. */
        long rel = fsize - copies_at;
        unsigned int copy = (unsigned int)(rel / (long)dsz);
        long in_copy = rel - (long)copy * (long)dsz;
        unsigned int block = (unsigned int)(in_copy / bs);
        if (block >= bc) block = bc - 1;
        if (flt) { flt->code = AP_E_COPY; flt->block = (int)block;
                   flt->offset = (unsigned int)fsize; }
        return AP_E_COPY;
    }
    if (sums_out) {
        unsigned int *s = (unsigned int*)malloc((size_t)bc * 4);
        if (!s) return AP_E_ARG;
        if (fseek(f, sums_at, SEEK_SET) != 0 || fread(s, 4, bc, f) != bc) {
            free(s);
            if (flt) { flt->code = AP_E_LENGTH; flt->block = -1; flt->offset = (unsigned int)sums_at; }
            return AP_E_LENGTH;
        }
        *sums_out = s;
    }
    lay->block_count = bc;
    lay->block_size = bs;
    lay->copies = cp;
    lay->data_at = (unsigned int)data_at;
    return 0;
}

/* --------------------------------------------------------------- write API */

static int ap_write_common(const char *path, const ApEntry *entries, int n,
                           const unsigned char *data, unsigned int data_size,
                           unsigned int block_size, int copies) {
    if (!path || (!entries && n > 0) || (!data && data_size > 0)) return AP_E_ARG;
    if (n < 0 || n > 65535) return AP_E_ARG;
    if (copies < 0 || copies > 64) return AP_E_ARG;
    if (copies > 0 && (block_size == 0 || data_size == 0)) return AP_E_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return AP_E_IO;
    unsigned short ver = (unsigned short)(copies > 0 ? AP_VERSION_CHECKED : AP_VERSION_LEGACY);
    if (fwrite(AP_MAGIC, 1, 4, f) != 4 || ap_write_u16(f, ver) != 0 ||
        ap_write_u16(f, (unsigned short)n) != 0) goto fail;
    for (int i = 0; i < n; ++i) {
        char name[32];
        memset(name, 0, sizeof(name));
        ap_copy_name(name, sizeof(name), entries[i].name);
        if (fwrite(name, 1, 32, f) != 32 ||
            ap_write_u32(f, entries[i].size) != 0 ||
            ap_write_u32(f, entries[i].offset) != 0) goto fail;
    }
    if (ap_write_u32(f, data_size) != 0) goto fail;
    if (copies > 0) {
        unsigned int bc = (data_size + block_size - 1) / block_size;
        if (ap_write_u32(f, bc) != 0 || ap_write_u32(f, block_size) != 0 ||
            ap_write_u32(f, (unsigned int)copies) != 0) goto fail;
        for (unsigned int b = 0; b < bc; ++b) {
            unsigned int off = b * block_size;
            unsigned int len = data_size - off < block_size ? data_size - off : block_size;
            if (ap_write_u32(f, ap_hash(data + off, len)) != 0) goto fail;
        }
    }
    if (data_size && fwrite(data, 1, data_size, f) != data_size) goto fail;
    for (int c = 0; c < copies; ++c)
        if (fwrite(data, 1, data_size, f) != data_size) goto fail;
    if (fclose(f) != 0) return AP_E_IO;
    return 0;
fail:
    fclose(f);
    remove(path);
    return AP_E_IO;
}

int ap_write(const char *path, const ApEntry *entries, int n,
             const unsigned char *data, unsigned int data_size) {
    return ap_write_common(path, entries, n, data, data_size, 0, 0);
}

int ap_write_checked(const char *path, const ApEntry *entries, int n,
                     const unsigned char *data, unsigned int data_size,
                     unsigned int block_size, int copies) {
    if (block_size == 0 || copies <= 0) return AP_E_ARG;
    return ap_write_common(path, entries, n, data, data_size, block_size, copies);
}

/* ---------------------------------------------------------------- read API */

int ap_read(const char *path, ApIndex *idx) {
    if (!path || !idx) return AP_E_ARG;
    memset(idx, 0, sizeof(*idx));
    FILE *f = fopen(path, "rb");
    if (!f) return AP_E_IO;
    unsigned int hdr = 0;
    int rc = ap_read_header(f, idx, &hdr);
    if (rc == 0 && idx->version == AP_VERSION_CHECKED) {
        long fsize = ap_file_size(f);
        ApLayout lay;
        ApFault flt;
        memset(&flt, 0, sizeof(flt));
        flt.block = -1;
        rc = fsize < 0 ? AP_E_IO : ap_read_layout(f, idx, hdr, fsize, &lay, NULL, &flt);
        if (rc == 0) idx->block_count = lay.block_count;
    }
    fclose(f);
    if (rc != 0) ap_free(idx);
    return rc;
}

/* Locate an entry; returns index or AP_E_NOTFOUND. */
static int ap_find(const ApIndex *idx, const char *name) {
    for (int i = 0; i < idx->count; ++i)
        if (strncmp(idx->entries[i].name, name, 31) == 0) return i;
    return AP_E_NOTFOUND;
}

/* Historic extractor: no verification, no repair, identical results. */
int ap_extract(const char *path, const char *name, unsigned char **out, unsigned int *out_size) {
    return ap_extract_ex(path, name, out, out_size, AP_LOAD_PLAIN, NULL);
}

/* ----------------------------------------------------------------- reports */

void ap_report_init(ApReport *rep) {
    if (!rep) return;
    memset(rep, 0, sizeof(*rep));
    rep->fault.code = 0;
    rep->fault.block = -1;
    rep->fault.entry = -1;
}

void ap_report_free(ApReport *rep) {
    if (!rep) return;
    free(rep->issues);
    rep->issues = NULL;
    rep->issue_count = 0;
}

static void ap_report_reset(ApReport *rep) {
    if (!rep) return;
    int bs = rep->block_size, bc = rep->block_count, cp = rep->copies;
    free(rep->issues);
    memset(rep, 0, sizeof(*rep));
    rep->block_size = bs; rep->block_count = bc; rep->copies = cp;
    rep->fault.block = -1;
    rep->fault.entry = -1;
}

static void ap_fault_set(ApFault *flt, int code, int block, int entry,
                         const char *name, unsigned int offset) {
    if (!flt) return;
    flt->code = code;
    flt->block = block;
    flt->entry = entry;
    flt->offset = offset;
    flt->name[0] = '\0';
    if (name) ap_copy_name(flt->name, sizeof(flt->name), name);
}

/* Describe the entries overlapping [off, off+len) of the data region. */
static void ap_describe_span(const ApIndex *idx, long off, unsigned int len,
                             int *count, int *first, const char **first_name) {
    long end = off + (long)len;
    int c = 0, f = -1;
    for (int i = 0; i < idx->count; ++i) {
        long e0 = idx->entries[i].offset;
        long e1 = e0 + (long)idx->entries[i].size;
        if (e0 < end && off < e1) {
            if (f < 0) f = i;
            ++c;
        }
    }
    if (count) *count = c;
    if (first) *first = f;
    if (first_name) *first_name = f >= 0 ? idx->entries[f].name : NULL;
}

typedef struct {
    ApIndex idx;
    unsigned int hdr;
    ApLayout lay;
    long fsize;
    unsigned int *sums;
    FILE *f;
    int writable; /* opened r+b; repair can rewrite the primary region */
} ApPack;

static void ap_pack_close(ApPack *p) {
    if (p->f) fclose(p->f);
    free(p->sums);
    p->f = NULL;
    p->sums = NULL;
    ap_free(&p->idx);
}

/* Open a v2 pack.  The file is opened read-write so that ap_repair_block can
 * write back; a read-only pack still verifies and extracts. */
static int ap_pack_open(const char *path, ApPack *p, ApFault *flt) {
    memset(p, 0, sizeof(*p));
    p->f = fopen(path, "r+b");
    p->writable = p->f != NULL;
    if (!p->f) p->f = fopen(path, "rb");
    if (!p->f) return AP_E_IO;
    int rc = ap_read_header(p->f, &p->idx, &p->hdr);
    if (rc != 0) { ap_pack_close(p); return rc; }
    if (p->idx.version != AP_VERSION_CHECKED) { ap_pack_close(p); return AP_E_VERSION; }
    p->fsize = ap_file_size(p->f);
    if (p->fsize < 0) { ap_pack_close(p); return AP_E_IO; }
    rc = ap_read_layout(p->f, &p->idx, p->hdr, p->fsize, &p->lay, &p->sums, flt);
    if (rc != 0) { ap_pack_close(p); return rc; }
    return 0;
}

static unsigned int ap_block_len(const ApPack *p, unsigned int block) {
    unsigned int off = block * p->lay.block_size;
    unsigned int n = p->idx.data_size - off;
    return n < p->lay.block_size ? n : p->lay.block_size;
}

/* Read block `block` from the primary data region (copy == -1) or from
 * redundant copy `copy`. */
static int ap_read_region(FILE *f, const ApPack *p, int copy,
                          unsigned int block, unsigned char *buf, unsigned int *len) {
    unsigned int off = block * p->lay.block_size;
    unsigned int n = ap_block_len(p, block);
    long base = copy < 0 ? (long)p->lay.data_at
                         : (long)p->lay.data_at + (long)p->idx.data_size +
                           (long)copy * (long)p->idx.data_size;
    if (fseek(f, base + (long)off, SEEK_SET) != 0) return AP_E_IO;
    if (fread(buf, 1, n, f) != n) return AP_E_LENGTH;
    *len = n;
    return 0;
}

static int ap_block_ok(FILE *f, const ApPack *p, int copy,
                       unsigned int block, unsigned char *buf) {
    unsigned int len = 0;
    int rc = ap_read_region(f, p, copy, block, buf, &len);
    return rc == 0 && len == ap_block_len(p, block) && ap_hash(buf, len) == p->sums[block];
}

/* First redundant copy that matches `block`, or -1.  The matching bytes are
 * left in `buf`. */
static int ap_find_copy(FILE *f, const ApPack *p, unsigned int block, unsigned char *buf) {
    for (unsigned int c = 0; c < p->lay.copies; ++c)
        if (ap_block_ok(f, p, (int)c, block, buf)) return (int)c;
    return -1;
}

/* Count redundant copies that match / fail for `block`. */
static void ap_copy_status(FILE *f, const ApPack *p, unsigned int block,
                           unsigned char *buf, int *ok, int *bad) {
    *ok = 0; *bad = 0;
    for (unsigned int c = 0; c < p->lay.copies; ++c) {
        if (ap_block_ok(f, p, (int)c, block, buf)) ++*ok; else ++*bad;
    }
}

/* Rewrite the primary data block from `good`. */
static int ap_store_block(ApPack *p, unsigned int block, const unsigned char *good) {
    unsigned int off = block * p->lay.block_size;
    unsigned int len = ap_block_len(p, block);
    if (!p->writable) return AP_E_READONLY;
    if (fseek(p->f, (long)p->lay.data_at + off, SEEK_SET) != 0) return AP_E_IO;
    if (fwrite(good, 1, len, p->f) != len) return AP_E_IO;
    if (fflush(p->f) != 0) return AP_E_IO;
    return 0;
}

static int ap_validate_entries(const ApIndex *idx, ApFault *flt) {
    for (int i = 0; i < idx->count; ++i) {
        unsigned int o = idx->entries[i].offset, s = idx->entries[i].size;
        if (o > idx->data_size || s > idx->data_size - o) {
            ap_fault_set(flt, AP_E_LENGTH, -1, i, idx->entries[i].name, o);
            return AP_E_LENGTH;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ stage 2: check */

int ap_check(const char *path, ApReport *rep) {
    if (!path || !rep) return AP_E_ARG;
    ap_report_reset(rep);
    ApPack p;
    ApFault flt;
    memset(&flt, 0, sizeof(flt));
    flt.block = -1; flt.entry = -1;
    int rc = ap_pack_open(path, &p, &flt);
    if (rc != 0) { rep->fault = flt; return rc; }
    rc = ap_validate_entries(&p.idx, &flt);
    if (rc != 0) { rep->fault = flt; ap_pack_close(&p); return rc; }

    rep->block_size = (int)p.lay.block_size;
    rep->block_count = (int)p.lay.block_count;
    rep->copies = (int)p.lay.copies;
    rep->entry_count = p.idx.count;

    unsigned char *buf = (unsigned char*)malloc(p.lay.block_size);
    if (!buf) { ap_pack_close(&p); return AP_E_ARG; }

    ApBlockIssue *issues = (ApBlockIssue*)calloc(p.lay.block_count, sizeof(ApBlockIssue));
    int *entry_bad = (int*)calloc((size_t)(p.idx.count ? p.idx.count : 1), sizeof(int));
    if (!issues || !entry_bad) {
        free(buf); free(issues); free(entry_bad); ap_pack_close(&p);
        return AP_E_ARG;
    }

    int nissues = 0;
    for (unsigned int b = 0; b < p.lay.block_count; ++b) {
        int primary_ok = ap_block_ok(p.f, &p, -1, b, buf);
        int cok = 0, cbad = 0;
        ap_copy_status(p.f, &p, b, buf, &cok, &cbad);

        if (primary_ok && cbad == 0) continue; /* block is whole */

        ApBlockIssue *is = &issues[nissues];
        memset(is, 0, sizeof(*is));
        unsigned int off = b * p.lay.block_size;
        unsigned int len = ap_block_len(&p, b);
        is->block = (int)b;
        is->primary_ok = primary_ok;
        is->copies_ok = cok;
        is->copies_bad = cbad;
        const char *fname = NULL;
        ap_describe_span(&p.idx, off, len, &is->entries, &is->first_entry, &fname);
        if (fname) ap_copy_name(is->first_name, sizeof(is->first_name), fname);
        if (!primary_ok) {
            ++rep->bad_primary;
            if (!cok) ++rep->unrecoverable;
            /* 1 = recoverable damage, 2 = unrecoverable */
            for (int e = 0; e < p.idx.count; ++e) {
                long e0 = p.idx.entries[e].offset;
                long e1 = e0 + (long)p.idx.entries[e].size;
                if (e0 < (long)off + (long)len && (long)off < e1)
                    entry_bad[e] = cok ? 1 : 2;
            }
        }
        ++nissues;
    }

    /* A truncated file was already rejected by ap_read_layout; reaching
     * here means every copy claimed by the layout is present. */
    rep->bad_copies = 0;
    for (int i = 0; i < nissues; ++i) rep->bad_copies += issues[i].copies_bad;

    /* affected_entries counts entries overlapping any bad primary block;
     * missing_entries counts those with at least one unrecoverable block. */
    int affected = 0, missing = 0;
    for (int e = 0; e < p.idx.count; ++e) {
        if (!entry_bad[e]) continue;
        ++affected;
        if (entry_bad[e] == 2) ++missing;
    }
    rep->affected_entries = affected;
    rep->missing_entries = missing;

    free(entry_bad);
    free(buf);
    rep->issues = issues;
    rep->issue_count = nissues;
    ap_pack_close(&p);
    return 0;
}

/* ------------------------------------------------- load with repair (stage 3) */

int ap_extract_ex(const char *path, const char *name,
                  unsigned char **out, unsigned int *out_size,
                  unsigned int flags, ApReport *rep) {
    if (!path || !name || !out || !out_size) return AP_E_ARG;
    *out = NULL;
    *out_size = 0;

    if (flags == AP_LOAD_PLAIN) {
        ApIndex idx;
        int rc = ap_read(path, &idx);
        if (rc != 0) return rc;
        int found = ap_find(&idx, name);
        if (found < 0) { ap_free(&idx); return AP_E_NOTFOUND; }
        FILE *f = fopen(path, "rb");
        if (!f) { ap_free(&idx); return AP_E_IO; }
        long pos = (long)ap_prefix_size(&idx) + (long)idx.entries[found].offset;
        unsigned int want = idx.entries[found].size;
        unsigned char *buf = (unsigned char*)malloc(want ? want : 1);
        if (!buf) { fclose(f); ap_free(&idx); return AP_E_ARG; }
        int ok = fseek(f, pos, SEEK_SET) == 0 && (want == 0 || fread(buf, 1, want, f) == want);
        fclose(f);
        ap_free(&idx);
        if (!ok) { free(buf); return AP_E_LENGTH; }
        *out = buf; *out_size = want;
        return 0;
    }

    if (!(flags & (AP_LOAD_VERIFY | AP_LOAD_REPAIR))) return AP_E_ARG;
    ApPack p;
    ApFault flt;
    memset(&flt, 0, sizeof(flt));
    flt.block = -1; flt.entry = -1;
    int rc = ap_pack_open(path, &p, &flt);
    if (rc != 0) { if (rep) { ap_report_reset(rep); rep->fault = flt; } return rc; }
    int found = ap_find(&p.idx, name);
    if (found < 0) { ap_pack_close(&p); return AP_E_NOTFOUND; }

    unsigned int esz = p.idx.entries[found].size;
    unsigned int eoff = p.idx.entries[found].offset;
    unsigned char *buf = (unsigned char*)malloc(esz ? esz : 1);
    unsigned char *blk = (unsigned char*)malloc(p.lay.block_size);
    if (!buf || !blk) { free(buf); free(blk); ap_pack_close(&p); return AP_E_ARG; }

    if (fseek(p.f, (long)p.lay.data_at + eoff, SEEK_SET) != 0 ||
        (esz && fread(buf, 1, esz, p.f) != esz)) {
        ap_fault_set(&flt, AP_E_LENGTH, esz ? (int)(eoff / p.lay.block_size) : -1,
                     found, p.idx.entries[found].name, eoff);
        if (rep) { ap_report_reset(rep); rep->fault = flt; }
        free(buf); free(blk); ap_pack_close(&p);
        return AP_E_LENGTH;
    }

    /* Verify every block the entry touches; splice repairs in as needed. */
    unsigned int first_b = eoff / p.lay.block_size;
    unsigned int last_b = esz ? (eoff + esz - 1) / p.lay.block_size : first_b - 1;
    int repaired = 0, unresolved = -1;
    if (esz) {
        for (unsigned int b = first_b; b <= last_b; ++b) {
            if (ap_block_ok(p.f, &p, -1, b, blk)) continue;
            int used = (flags & AP_LOAD_REPAIR) ? ap_find_copy(p.f, &p, b, blk) : -1;
            if (used < 0) { unresolved = (int)b; break; }
            /* ap_find_copy left the matching copy's bytes in blk. */
            unsigned int bstart = b * p.lay.block_size;
            unsigned int len = ap_block_len(&p, b);
            for (unsigned int k = 0; k < len; ++k) {
                unsigned int abs = bstart + k;
                if (abs >= eoff && abs < eoff + esz) buf[abs - eoff] = blk[k];
            }
            ++repaired;
        }
    }

    if (unresolved >= 0) {
        ap_fault_set(&flt, (flags & AP_LOAD_REPAIR) ? AP_E_COPY : AP_E_CORRUPT,
                     unresolved, found, p.idx.entries[found].name,
                     (unsigned int)unresolved * p.lay.block_size);
        if (rep) {
            ap_report_reset(rep);
            rep->block_size = (int)p.lay.block_size;
            rep->block_count = (int)p.lay.block_count;
            rep->copies = (int)p.lay.copies;
            rep->fault = flt;
        }
        free(buf); free(blk); ap_pack_close(&p);
        return flt.code;
    }

    if (rep) {
        ap_report_reset(rep);
        rep->block_size = (int)p.lay.block_size;
        rep->block_count = (int)p.lay.block_count;
        rep->copies = (int)p.lay.copies;
        rep->entry_count = p.idx.count;
        rep->repaired = repaired;
    }
    free(blk);
    ap_pack_close(&p);
    *out = buf; *out_size = esz;
    return 0;
}

/* -------------------------------------------------------------- stage 3: repair */

int ap_repair_block(const char *path, int block, ApReport *rep) {
    if (!path) return AP_E_ARG;
    ApPack p;
    ApFault flt;
    memset(&flt, 0, sizeof(flt));
    flt.block = -1; flt.entry = -1;
    int rc = ap_pack_open(path, &p, &flt);
    if (rc != 0) { if (rep) { ap_report_reset(rep); rep->fault = flt; } return rc; }
    unsigned int nblocks = p.lay.block_count;
    if (rep) {
        ap_report_reset(rep);
        rep->block_size = (int)p.lay.block_size;
        rep->block_count = (int)nblocks;
        rep->copies = (int)p.lay.copies;
        rep->entry_count = p.idx.count;
    }
    if (block < 0 || (unsigned int)block >= nblocks) {
        ap_fault_set(&flt, AP_E_BLOCK, block, -1, NULL, 0);
        if (rep) rep->fault = flt;
        ap_pack_close(&p);
        return AP_E_BLOCK;
    }

    unsigned int off = (unsigned int)block * p.lay.block_size;
    unsigned int len = ap_block_len(&p, (unsigned int)block);
    int count = 0, first = -1;
    const char *name = NULL;
    ap_describe_span(&p.idx, off, len, &count, &first, &name);

    unsigned char *buf = (unsigned char*)malloc(len ? len : 1);
    unsigned char *good = (unsigned char*)malloc(len ? len : 1);
    if (!buf || !good) { free(buf); free(good); ap_pack_close(&p); return AP_E_ARG; }

    int primary_ok = ap_block_ok(p.f, &p, -1, (unsigned int)block, buf);
    int cok = 0, cbad = 0;
    ap_copy_status(p.f, &p, (unsigned int)block, buf, &cok, &cbad);
    int repaired = 0;
    if (!primary_ok) {
        int copy = ap_find_copy(p.f, &p, (unsigned int)block, good);
        if (copy < 0) {
            ap_fault_set(&flt, AP_E_COPY, block, first, name, off);
            if (rep) {
                rep->fault = flt;
                rep->bad_primary = 1;
                rep->bad_copies = cbad;
                rep->unrecoverable = 1;
                if (count) rep->affected_entries = count;
            }
            free(buf); free(good); ap_pack_close(&p);
            return AP_E_COPY;
        }
        rc = ap_store_block(&p, (unsigned int)block, good);
        if (rc != 0) { free(buf); free(good); ap_pack_close(&p); return rc; }
        repaired = 1;
    }
    if (rep) {
        rep->repaired = repaired;
        rep->bad_primary = primary_ok ? 0 : 1;
        rep->bad_copies = cbad;
        if (!primary_ok || cbad) {
            rep->issues = (ApBlockIssue*)calloc(1, sizeof(ApBlockIssue));
            if (rep->issues) {
                rep->issues[0].block = block;
                rep->issues[0].entries = count;
                rep->issues[0].first_entry = first;
                if (name) ap_copy_name(rep->issues[0].first_name,
                                       sizeof(rep->issues[0].first_name), name);
                rep->issues[0].primary_ok = primary_ok;
                rep->issues[0].copies_ok = cok;
                rep->issues[0].copies_bad = cbad;
                rep->issues[0].repaired = repaired;
                rep->issue_count = 1;
                if (!primary_ok && count) rep->affected_entries = count;
            }
        }
    }
    free(buf); free(good);
    ap_pack_close(&p);
    return 0;
}

/* Restore every repairable block; report the first one that has no copy. */
int ap_repair(const char *path, ApReport *rep) {
    if (!path) return AP_E_ARG;
    ApReport scan;
    ap_report_init(&scan);
    int rc = ap_check(path, &scan);
    if (rc != 0) {
        if (rep) { ap_report_reset(rep); rep->fault = scan.fault; }
        ap_report_free(&scan);
        return rc;
    }
    int repaired = 0;
    int fail_block = -1, fail_entry = -1;
    char fail_name[32] = "";
    unsigned int fail_off = 0;
    for (int i = 0; i < scan.issue_count; ++i) {
        if (scan.issues[i].primary_ok) continue;
        ApReport one;
        ap_report_init(&one);
        int brc = ap_repair_block(path, scan.issues[i].block, &one);
        if (brc == 0) {
            repaired += one.repaired;
        } else if (brc == AP_E_COPY || brc == AP_E_BLOCK) {
            if (fail_block < 0) {
                fail_block = scan.issues[i].block;
                fail_entry = scan.issues[i].first_entry;
                ap_copy_name(fail_name, sizeof(fail_name), scan.issues[i].first_name);
                fail_off = (unsigned int)scan.issues[i].block * (unsigned int)scan.block_size;
            }
        } else {
            if (rep) { ap_report_reset(rep); rep->fault = one.fault; }
            ap_report_free(&one);
            ap_report_free(&scan);
            return brc;
        }
        ap_report_free(&one);
    }
    if (rep) {
        ap_report_reset(rep);
        rep->block_size = scan.block_size;
        rep->block_count = scan.block_count;
        rep->copies = scan.copies;
        rep->entry_count = scan.entry_count;
        rep->bad_primary = scan.bad_primary;
        rep->bad_copies = scan.bad_copies;
        rep->repaired = repaired;
        rep->unrecoverable = scan.unrecoverable;
        rep->affected_entries = scan.affected_entries;
        rep->missing_entries = scan.missing_entries;
        if (fail_block >= 0)
            ap_fault_set(&rep->fault, AP_E_COPY, fail_block, fail_entry,
                         fail_name[0] ? fail_name : NULL, fail_off);
    }
    ap_report_free(&scan);
    return fail_block >= 0 ? AP_E_COPY : 0;
}

void ap_free(ApIndex *idx) { if (idx) { free(idx->entries); idx->entries = NULL; idx->count = 0; } }
