#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

int main(void) {
    unsigned char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ApEntry e[2];
    memset(e, 0, sizeof(e));
    strcpy(e[0].name, "a.bin"); e[0].size = 4; e[0].offset = 0;
    strcpy(e[1].name, "b.bin"); e[1].size = 4; e[1].offset = 4;
    CK(ap_write("/tmp/t.gpak", e, 2, data, 8) == 0);
    ApIndex idx;
    CK(ap_read("/tmp/t.gpak", &idx) == 0);
    CK(idx.count == 2 && idx.data_size == 8);
    CK(strcmp(idx.entries[1].name, "b.bin") == 0);
    ap_free(&idx);
    unsigned char *buf = NULL; unsigned int sz = 0;
    CK(ap_extract("/tmp/t.gpak", "b.bin", &buf, &sz) == 0);
    CK(sz == 4 && buf[0] == 5);
    free(buf);
    CK(ap_extract("/tmp/t.gpak", "zzz", &buf, &sz) == -5);
    CK(ap_write("/tmp/bad.gpak", e, -1, data, 8) == -1);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
