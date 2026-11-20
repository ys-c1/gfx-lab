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
