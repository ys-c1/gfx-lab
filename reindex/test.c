#include "reindex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

int main(void) {
    RxErr err = {0, 0, 0};

    /* ---- 阶段一：建映射表（一轮轮换 perm[i]=i+1 mod n） ---- */
    enum { N = 8 };
    int perm[N];
    for (int i = 0; i < N; ++i) perm[i] = (i + 1) % N;
    RxTable t;
    CK(rx_build(perm, N, &t, &err) == RX_OK);
    CK(t.n == N && t.conflicts == 0);
    for (int i = 0; i < N; ++i) CK(t.old_to_new[i] == (i + 1) % N);

    /* ---- 阶段二：批量改写引用 ---- */
    int refs[5] = {0, 1, 2, 7, 3};
    int expect[5] = {1, 2, 3, 0, 4};
    int clash = -1;
    int rw = rx_rewrite(&t, refs, 5, &clash, &err);
    CK(rw == 5 && clash == 0);
    for (int i = 0; i < 5; ++i) CK(refs[i] == expect[i]);

    /* 不改写时旧引用确实指向错误的图（演示问题根因）：
       旧槽位 7 的图现在在槽位 0，若沿用旧索引 7 就取到另一张图。 */
    int raw[1] = {7};
    CK(raw[0] != t.old_to_new[7]);

    /* ---- 越界引用计冲突、保留原值 ---- */
    int bad_refs[3] = {2, 99, 4};
    clash = -1;
    rw = rx_rewrite(&t, bad_refs, 3, &clash, &err);
    CK(rw == 2 && clash == 1);
    CK(bad_refs[1] == 99);          /* 冲突项原样保留 */
    CK(bad_refs[0] == 3 && bad_refs[2] == 5);

    /* ---- 负数 refs：硬报错且指出第几项 ---- */
    int neg_refs[3] = {1, -4, 2};
    err.code = RX_OK;
    rw = rx_rewrite(&t, neg_refs, 3, &clash, &err);
    CK(rw == RX_EINVAL);
    CK(err.item == 1 && strcmp(err.param, "refs") == 0);

    /* ---- 负数 perm / 越界 perm：分别报错并指出项 ---- */
    int bad_perm[N];
    memcpy(bad_perm, perm, sizeof(perm));
    bad_perm[3] = -1;
    RxTable t2;
    err.code = RX_OK;
    CK(rx_build(bad_perm, N, &t2, &err) == RX_EINVAL);
    CK(err.item == 3 && strcmp(err.param, "perm") == 0);

    memcpy(bad_perm, perm, sizeof(perm));
    bad_perm[3] = N + 5;
    err.code = RX_OK;
    CK(rx_build(bad_perm, N, &t2, &err) == RX_ERANGE);
    CK(err.item == 3);

    /* ---- 标量负数 ---- */
    err.code = RX_OK;
    int dummy[2] = {0, 1};
    CK(rx_rewrite(&t, dummy, -2, &clash, &err) == RX_EINVAL);
    CK(err.item == -1 && strcmp(err.param, "m") == 0);
    err.code = RX_OK;
    CK(rx_build(perm, -3, &t2, &err) == RX_EINVAL);
    CK(err.item == -1 && strcmp(err.param, "n") == 0);

    /* ---- 建表冲突：新槽位被重复认领 ---- */
    int dup_perm[4] = {2, 2, 0, 1}; /* 新槽位 2 被旧 0、旧 1 同时认领 */
    RxTable t3;
    CK(rx_build(dup_perm, 4, &t3, &err) == RX_OK);
    CK(t3.conflicts == 1);
    CK(t3.old_to_new[0] == 2);      /* 先认领者生效 */
    CK(t3.old_to_new[1] == -1);     /* 后者不建映射 */
    int crefs[2] = {1, 2};
    clash = -1;
    rw = rx_rewrite(&t3, crefs, 2, &clash, &err);
    CK(rw == 1 && clash == 1);     /* 命中冲突槽位的引用计冲突、保留 */
    CK(crefs[0] == 1 && crefs[1] == 0);

    /* ---- 导出映射表 CSV ---- */
    const char *csv = "/tmp/reindex_map_test.csv";
    CK(rx_export(&t, csv) == RX_OK);
    FILE *f = fopen(csv, "r");
    CK(f != NULL);
    char line[128];
    int got_header = 0, got_old7 = 0, rows = 0;
    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "old,new", 7) == 0) got_header = 1;
        if (strncmp(line, "7,0", 3) == 0) got_old7 = 1;
        if (line[0] >= '0' && line[0] <= '9') ++rows;
    }
    if (f) fclose(f);
    CK(got_header && got_old7 && rows == N);

    /* ---- 开关关闭：输出与旧路径逐字节一致 ---- */
    int base[6] = {5, 0, 7, 2, 6, 1};
    int off[6], on[6];
    memcpy(off, base, sizeof(base));
    memcpy(on, base, sizeof(base));
    int r_on = 0, c_on = 0;
    CK(rx_apply(perm, N, off, 6, 0, NULL, NULL, NULL, &err) == RX_OK);
    CK(memcmp(off, base, sizeof(base)) == 0);   /* 关闭路径零改动 */
    CK(rx_apply(perm, N, on, 6, 1, NULL, &r_on, &c_on, &err) == RX_OK);
    CK(r_on == 6 && c_on == 0);
    for (int i = 0; i < 6; ++i) CK(on[i] == (base[i] + 1) % N);

    /* 负的开关值同样报错并指出参数名 */
    err.code = RX_OK;
    CK(rx_apply(perm, N, on, 6, -1, NULL, NULL, NULL, &err) == RX_EINVAL);
    CK(strcmp(err.param, "enabled") == 0);

    /* ---- 空表 / n=0 边界 ---- */
    RxTable zero;
    CK(rx_build(perm, 0, &zero, &err) == RX_OK && zero.n == 0);
    rx_free(&zero);
    CK(rx_build(NULL, N, &t2, &err) == RX_EINVAL);

    rx_free(&t); rx_free(&t3);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
