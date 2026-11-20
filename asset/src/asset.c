#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AP_MAGIC "GPAK"
#define AP_VERSION 1

int ap_write(const char *path, const ApEntry *entries, int n, const unsigned char *data, unsigned int data_size) {
    if (!path || (!entries && n > 0) || (!data && data_size > 0)) return -1;
    if (n < 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    unsigned short ver = AP_VERSION, cnt = (unsigned short)n;
    fwrite(AP_MAGIC, 1, 4, f);
    fwrite(&ver, 2, 1, f);
    fwrite(&cnt, 2, 1, f);
    for (int i = 0; i < n; ++i) {
        char name[32];
        memset(name, 0, sizeof(name));
        strncpy(name, entries[i].name, sizeof(name) - 1);
        fwrite(name, 1, 32, f);
        fwrite(&entries[i].size, 4, 1, f);
        fwrite(&entries[i].offset, 4, 1, f);
    }
    fwrite(&data_size, 4, 1, f);
    if (data_size) fwrite(data, 1, data_size, f);
    fclose(f);
    return 0;
}

int ap_read(const char *path, ApIndex *idx) {
    if (!path || !idx) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -2;
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, AP_MAGIC, 4) != 0) { fclose(f); return -3; }
    unsigned short ver = 0, cnt = 0;
    if (fread(&ver, 2, 1, f) != 1 || ver != AP_VERSION) { fclose(f); return -4; }
    if (fread(&cnt, 2, 1, f) != 1) { fclose(f); return -3; }
    idx->entries = (ApEntry*)calloc(cnt ? cnt : 1, sizeof(ApEntry));
    if (!idx->entries) { fclose(f); return -1; }
    idx->count = cnt;
    for (int i = 0; i < cnt; ++i) {
        if (fread(idx->entries[i].name, 1, 32, f) != 32) { fclose(f); return -3; }
        if (fread(&idx->entries[i].size, 4, 1, f) != 1) { fclose(f); return -3; }
        if (fread(&idx->entries[i].offset, 4, 1, f) != 1) { fclose(f); return -3; }
    }
    if (fread(&idx->data_size, 4, 1, f) != 1) { fclose(f); return -3; }
    fclose(f);
    return 0;
}

int ap_extract(const char *path, const char *name, unsigned char **out, unsigned int *out_size) {
    if (!path || !name || !out || !out_size) return -1;
    ApIndex idx;
    int rc = ap_read(path, &idx);
    if (rc != 0) return rc;
    int found = -1;
    for (int i = 0; i < idx.count; ++i)
        if (strncmp(idx.entries[i].name, name, 31) == 0) { found = i; break; }
    if (found < 0) { ap_free(&idx); return -5; }
    FILE *f = fopen(path, "rb");
    if (!f) { ap_free(&idx); return -2; }
    unsigned int header = 4 + 2 + 2 + (unsigned int)idx.count * 40 + 4;
    fseek(f, (long)(header + idx.entries[found].offset), SEEK_SET);
    *out_size = idx.entries[found].size;
    *out = (unsigned char*)malloc(*out_size ? *out_size : 1);
    if (*out && *out_size) fread(*out, 1, *out_size, f);
    fclose(f);
    ap_free(&idx);
    return 0;
}

void ap_free(ApIndex *idx) { if (idx) { free(idx->entries); idx->entries = NULL; idx->count = 0; } }

unsigned long long ap_hash(const unsigned char *p, unsigned int n) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned int i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

int ap_pack(const char *path, const ApEntry *entries, int n, const unsigned char *data,
            unsigned int data_size, int dedup, ApHashFn hf, int *err_entry) {
    if (err_entry) *err_entry = -1;
    if (!dedup) return ap_write(path, entries, n, data, data_size);
    if (!path || (!entries && n > 0) || (!data && data_size > 0)) return -1;
    if (n <= 0) return AP_ERR_EMPTY;
    if (!hf) hf = ap_hash;
    int rc = 0;
    unsigned long long *hs = (unsigned long long*)malloc((size_t)n * sizeof(*hs));
    int *canon = (int*)malloc((size_t)n * sizeof(*canon));
    unsigned int *noff = (unsigned int*)malloc((size_t)n * sizeof(*noff));
    ApEntry *ne = (ApEntry*)malloc((size_t)n * sizeof(*ne));
    unsigned char *blob = (unsigned char*)malloc(data_size ? data_size : 1);
    if (!hs || !canon || !noff || !ne || !blob) { rc = -1; goto out; }

    /* 阶段一：按内容哈希合并重复项 */
    for (int i = 0; i < n; ++i) {
        if (entries[i].name[0] == '\0') { if (err_entry) *err_entry = i; rc = AP_ERR_ALIAS; goto out; }
        if (entries[i].offset > data_size || entries[i].size > data_size - entries[i].offset) {
            if (err_entry) *err_entry = i;
            rc = -1;
            goto out;
        }
        canon[i] = -1;
        hs[i] = hf(data + entries[i].offset, entries[i].size);
        for (int j = 0; j < i; ++j) {
            if (hs[j] != hs[i]) continue;
            int cj = canon[j] < 0 ? j : canon[j];
            if (entries[cj].size == entries[i].size &&
                memcmp(data + entries[cj].offset, data + entries[i].offset, entries[i].size) == 0) {
                canon[i] = cj; /* 内容相同：合并到代表条目 */
                break;
            }
            if (err_entry) *err_entry = i; /* 哈希相同、内容不同：冲突 */
            rc = AP_ERR_HASH;
            goto out;
        }
    }

    /* 阶段二：重映射索引并保留别名 */
    {
        unsigned int w = 0;
        for (int i = 0; i < n; ++i) {
            if (canon[i] >= 0) continue;
            noff[i] = w;
            memcpy(blob + w, data + entries[i].offset, entries[i].size);
            w += entries[i].size;
        }
        for (int i = 0; i < n; ++i) {
            int c = canon[i] < 0 ? i : canon[i];
            ne[i] = entries[i]; /* 名字保留：别名仍可查找 */
            ne[i].offset = noff[c];
            ne[i].size = entries[c].size;
        }
        rc = ap_write(path, ne, n, blob, w);
    }
out:
    free(hs); free(canon); free(noff); free(ne); free(blob);
    return rc;
}
