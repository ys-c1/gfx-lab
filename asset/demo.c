#include "asset.h"
#include "apack.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    unsigned char data[64];
    for (int i = 0; i < 64; ++i) data[i] = (unsigned char)(i * 3);
    ApBlock blocks[3] = {{8, 1}, {8, 16}, {12, 8}};
    const char *names[3] = {"tex.bin", "idx.bin", "hdr.bin"};
    unsigned int off_g[3], tot_g = 0, off_t[3], tot_t = 0;
    if (ap_pack_plan(blocks, 3, AP_PACK_GREEDY, off_g, &tot_g, NULL) != 0) return 1;
    if (ap_pack_plan(blocks, 3, AP_PACK_TIGHT, off_t, &tot_t, NULL) != 0) return 1;
    printf("greedy %u bytes, tight %u bytes\n", tot_g, tot_t);
    ApEntry e[3];
    memset(e, 0, sizeof(e));
    for (int i = 0; i < 3; ++i) { strcpy(e[i].name, names[i]); e[i].size = blocks[i].size; }
    ap_pack_apply(e, off_t, 3); /* 阶段二: 回填索引 */
    if (ap_write("demo.gpak", e, 3, data, tot_t) != 0) return 1;
    ApIndex idx;
    if (ap_read("demo.gpak", &idx) != 0) return 1;
    printf("entries %d data %u\n", idx.count, idx.data_size);
    for (int i = 0; i < idx.count; ++i)
        printf("  %s %u @%u\n", idx.entries[i].name, idx.entries[i].size, idx.entries[i].offset);
    ap_free(&idx);
    return 0;
}
