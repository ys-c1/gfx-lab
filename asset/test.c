#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static unsigned long long hash_const(const unsigned char *p, unsigned int n) { (void)p; (void)n; return 1; }

static int files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    int eq = 1;
    for (;;) { int ca = fgetc(fa), cb = fgetc(fb); if (ca != cb) { eq = 0; break; } if (ca == EOF) break; }
    fclose(fa); fclose(fb);
    return eq;
}

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

    /* --- 去重打包 --- */
    unsigned char d2[12] = {9, 9, 9, 9, 1, 2, 3, 4, 9, 9, 9, 9}; /* x 与 z 内容相同 */
    ApEntry e3[3];
    memset(e3, 0, sizeof(e3));
    strcpy(e3[0].name, "x"); e3[0].size = 4; e3[0].offset = 0;
    strcpy(e3[1].name, "y"); e3[1].size = 4; e3[1].offset = 4;
    strcpy(e3[2].name, "z"); e3[2].size = 4; e3[2].offset = 8;
    int err = -99;
    CK(ap_pack("/tmp/d1.gpak", e3, 3, d2, 12, 1, NULL, &err) == 0);
    ApIndex di;
    CK(ap_read("/tmp/d1.gpak", &di) == 0);
    CK(di.count == 3);                 /* 别名保留：三个名字都在索引里 */
    CK(di.data_size == 8);             /* 重复内容只存一份 */
    CK(strcmp(di.entries[2].name, "z") == 0);
    ap_free(&di);
    unsigned char *b1 = NULL, *b2 = NULL; unsigned int s1 = 0, s2 = 0;
    CK(ap_extract("/tmp/d1.gpak", "x", &b1, &s1) == 0);
    CK(ap_extract("/tmp/d1.gpak", "z", &b2, &s2) == 0);
    CK(s1 == 4 && s2 == 4 && memcmp(b1, b2, 4) == 0 && b1[0] == 9);
    free(b1); free(b2);

    /* 关闭去重：输出与 ap_write 逐字节一致，读取结果相同 */
    CK(ap_pack("/tmp/o1.gpak", e3, 3, d2, 12, 0, NULL, NULL) == 0);
    CK(ap_write("/tmp/o2.gpak", e3, 3, d2, 12) == 0);
    CK(files_equal("/tmp/o1.gpak", "/tmp/o2.gpak"));

    /* 条目数为零：报错 */
    err = -99;
    CK(ap_pack("/tmp/e0.gpak", e3, 0, d2, 12, 1, NULL, &err) == AP_ERR_EMPTY);

    /* 别名为空：报错并指明条目 */
    ApEntry e4[2];
    memset(e4, 0, sizeof(e4));
    strcpy(e4[0].name, "ok"); e4[0].size = 4; e4[0].offset = 0;
    e4[1].name[0] = '\0';    e4[1].size = 4; e4[1].offset = 4;
    err = -99;
    CK(ap_pack("/tmp/e1.gpak", e4, 2, d2, 12, 1, NULL, &err) == AP_ERR_ALIAS);
    CK(err == 1);

    /* 哈希冲突：注入常数哈希，内容不同 → 报错并指明条目 */
    err = -99;
    CK(ap_pack("/tmp/h1.gpak", e3, 3, d2, 12, 1, hash_const, &err) == AP_ERR_HASH);
    CK(err == 1);                      /* 条目 1 (y) 与 x 哈希同、内容异 */

    /* 注入常数哈希但内容确实相同：正常合并，不误报冲突 */
    ApEntry e5[2];
    memset(e5, 0, sizeof(e5));
    strcpy(e5[0].name, "p"); e5[0].size = 4; e5[0].offset = 0;
    strcpy(e5[1].name, "q"); e5[1].size = 4; e5[1].offset = 8;
    err = -99;
    CK(ap_pack("/tmp/h2.gpak", e5, 2, d2, 12, 1, hash_const, &err) == 0);
    CK(ap_read("/tmp/h2.gpak", &di) == 0);
    CK(di.count == 2 && di.data_size == 4);
    ap_free(&di);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
