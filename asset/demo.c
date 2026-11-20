#include "asset.h"
#include <stdio.h>
#include <string.h>

static void show(const char *tag, const ApIndex *idx) {
    printf("%s: version=%u count=%d data=%u migrated=%d backfilled=%d\n",
           tag, idx->version, idx->count, idx->data_size, idx->migrated, idx->backfilled);
    for (int i = 0; i < idx->count; ++i)
        printf("  %-8s size=%u offset=%u flags=0x%x crc=%08x\n",
               idx->entries[i].name, idx->entries[i].size, idx->entries[i].offset,
               idx->entries[i].flags, idx->entries[i].crc);
}

int main(void) {
    unsigned char data[16];
    for (int i = 0; i < 16; ++i) data[i] = (unsigned char)(i * 3);
    ApEntry e[2];
    memset(e, 0, sizeof(e));
    strcpy(e[0].name, "tex.bin"); e[0].size = 8; e[0].offset = 0;
    strcpy(e[1].name, "idx.bin"); e[1].size = 8; e[1].offset = 8;

    /* 新版写 v2 包 */
    if (ap_write("demo.gpak", e, 2, data, 16) != 0) return 1;
    ApIndex idx;
    if (ap_read("demo.gpak", &idx) != 0) return 1;
    show("v2 native", &idx);
    ap_free(&idx);

    /* 造一个 v1 旧包：迁移关闭时拒绝，迁移开启时补齐为 v2 结构 */
    if (ap_write_v1("build/legacy.gpak", e, 2, data, 16) != 0) return 1;
    ApError err;
    int rc = ap_read("build/legacy.gpak", &idx);
    printf("v1 with migration off -> %d (expected %d)\n", rc, AP_ERR_VERSION);

    rc = ap_readx("build/legacy.gpak", AP_READ_MIGRATE, &idx, &err);
    if (rc != 0) { printf("migration failed: %s\n", err.detail); return 1; }
    show("v1 migrated", &idx);
    ap_free(&idx);
    return 0;
}
