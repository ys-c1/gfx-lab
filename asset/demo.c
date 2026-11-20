#include "asset.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    unsigned char data[16];
    for (int i = 0; i < 16; ++i) data[i] = (unsigned char)(i * 3);
    ApEntry e[2];
    memset(e, 0, sizeof(e));
    strcpy(e[0].name, "tex.bin"); e[0].size = 8; e[0].offset = 0;
    strcpy(e[1].name, "idx.bin"); e[1].size = 8; e[1].offset = 8;
    if (ap_write("demo.gpak", e, 2, data, 16) != 0) return 1;
    ApIndex idx;
    if (ap_read("demo.gpak", &idx) != 0) return 1;
    printf("entries %d data %u\n", idx.count, idx.data_size);
    for (int i = 0; i < idx.count; ++i) printf("  %s %u\n", idx.entries[i].name, idx.entries[i].size);
    ap_free(&idx);
    return 0;
}
