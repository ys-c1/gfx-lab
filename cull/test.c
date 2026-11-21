#include "cull.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int same_as_brute(const CullBox *b, int n, CullView v, const CullHashResult *h) {
    CullResult r;
    if (cull_run(b, n, v, &r) != 0) return 0;
    int ok = (r.count == h->count);
    /* cull_run outputs ascending indices, so compare position by position. */
    for (int i = 0; ok && i < r.count; ++i) ok = (r.ids[i] == h->ids[i]);
    free(r.ids);
    return ok;
}

static unsigned long long rng_state = 0x2545F4914F6CDD1DULL;
static unsigned long long rnd(void) {
    unsigned long long x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return x;
}
static double frand(double lo, double hi) {
    return lo + (hi - lo) * (double)(rnd() >> 11) / (double)(1ULL << 53);
}

static void test_legacy_unchanged(void) {
    CullBox b[4] = {{0,0,10,10,0},{100,100,10,10,1},{50,50,20,20,0},{-5,-5,10,10,2}};
    CullView v = {0, 0, 60, 60};
    CullResult base;
    CK(cull_run(b, 4, v, &base) == 0);
    CK(base.count == 3);
    CK(base.ids[0] == 0 && base.ids[1] == 2 && base.ids[2] == 3);
    CK(cull_count_within(b, 4, v) == 3);

    /* 关闭新分支：输出逐项照旧（顺序、数量、id 全部相同） */
    CullHashOpts o;
    memset(&o, 0, sizeof o);
    o.legacy = 1;
    CullHashResult lr;
    CullHashError err;
    CK(cull_run_hashed(b, 4, v, &o, &lr, &err) == CULL_OK);
    CK(lr.legacy == 1);
    CK(lr.count == base.count);
    CK(memcmp(lr.ids, base.ids, sizeof(int) * (size_t)base.count) == 0);
    cull_hash_result_free(&lr);

    /* 相同输入走哈希路径也必须与旧输出逐项相同 */
    o.legacy = 0;
    CK(cull_run_hashed(b, 4, v, &o, &lr, &err) == CULL_OK);
    CK(lr.legacy == 0);
    CK(memcmp(lr.ids, base.ids, sizeof(int) * (size_t)base.count) == 0);
    cull_hash_result_free(&lr);
    free(base.ids);

    CullView bad = {0, 0, 0, 10};
    CK(cull_run(b, 4, bad, &base) == -2);
    CK(cull_run_hashed(b, 4, bad, &o, &lr, &err) == CULL_ERR_VIEW);
    CK(err.code == CULL_ERR_VIEW && err.field && strcmp(err.field, "view") == 0);
}

static void test_two_stage_and_stats(void) {
    /* 4x4 网格，单元 10x10，显式指定便于手算桶号。 */
    enum { N = 8 };
    CullBox b[N] = {
        {0,0,10,10,0},      /* 0: (0,0) 单桶 */
        {10,0,10,10,0},     /* 1: (1,0) */
        {0,10,10,10,0},     /* 2: (0,1) */
        {95,95,10,10,0},    /* 3: (9,9) 右下角 */
        {5,5,10,10,0},      /* 4: 跨 (0,0)-(1,1) 四桶 */
        {30,30,60,60,0},    /* 5: (3,3)-(8,8) 6x6 桶 */
        {200,200,10,10,0},  /* 6: 视口外 */
        {0,0,100,100,0},    /* 7: 全域 */
    };
    CullHashOpts o;
    memset(&o, 0, sizeof o);
    o.cell_w = 10; o.cell_h = 10; o.max_buckets = 500;
    CullHash *h = NULL;
    CullHashError err;
    CK(cull_hash_build(b, N, &o, &h, &err) == CULL_OK);
    CK(h != NULL);

    CullHashStats s;
    CK(cull_hash_get_stats(h, &s) == CULL_OK);
    /* 世界范围 0..210 x 0..210，单元 10 -> 21x21 = 441 桶。
     * 闭区间语义：压线的盒会同时进相邻桶（这样边界相接才不漏判）。 */
    CK(s.buckets == 441);
    /* 闭区间语义：压线的盒同时进两侧桶，保证“边界相接”的对象对不会漏判。
     * 盒 0..4 各 2x2 桶，盒 5 为 7x7，盒 6 夹到最右列/行 1 桶，盒 7 为 11x11。 */
    CK(s.entries == 4+4+4+4+4+49+1+121);
    CK(s.occupied == 122); /* 盒 7 的 11x11 网格，加上盒 6 的 (20,20) */
    CK(s.collisions == s.entries - s.occupied);
    double expect = 100.0 * (double)s.collisions / (double)s.entries;
    CK(fabs(s.collision_rate - expect) < 1e-12);

    CullView v = {0, 0, 25, 25};
    CullHashResult r;
    CK(cull_hash_query(h, v, &r, &err) == CULL_OK);
    /* 视口覆盖桶 (0..2, 0..2)：命中 0,1,2,4,7；3 在 (9,10)，5 在 (3..9)，
     * 6 在 (20,20)，均不在视口桶内 */
    CK(r.count == 5);
    CK(r.ids[0]==0 && r.ids[1]==1 && r.ids[2]==2 && r.ids[3]==4 && r.ids[4]==7);
    /* 候选：被视口覆盖桶的去重对象；小于全量 N，且不少于可见数 */
    CK(r.candidates == 5);
    CK(r.candidates >= r.count);
    CK(r.visited_buckets == 9);
    CK(r.stats.buckets == 441 && r.stats.collisions == s.collisions);
    CK(same_as_brute(b, N, v, &r));
    cull_hash_result_free(&r);

    /* 视口完全在网格外：零桶访问、零候选、零可见 */
    CullView outside = {-500, -500, 10, 10};
    CK(cull_hash_query(h, outside, &r, &err) == CULL_OK);
    CK(r.count == 0 && r.candidates == 0 && r.visited_buckets == 0);
    cull_hash_result_free(&r);

    /* 视口大于世界：访问全部桶，全部对象成为候选 */
    CullView all = {-100, -100, 1000, 1000};
    CK(cull_hash_query(h, all, &r, &err) == CULL_OK);
    CK(r.count == N);
    CK(r.candidates == N);
    CK(r.visited_buckets == s.buckets);
    cull_hash_result_free(&r);

    cull_hash_free(h);
}

static void test_boundaries(void) {
    /* 盒子正好落在单元边界上：闭区间语义下不能漏桶。 */
    CullBox b[3] = {{10,10,10,10,0},{0,0,10,10,0},{20,20,10,10,0}};
    CullHashOpts o;
    memset(&o, 0, sizeof o);
    o.cell_w = 10; o.cell_h = 10;
    CullHash *h = NULL;
    CullHashError err;
    CK(cull_hash_build(b, 3, &o, &h, &err) == CULL_OK);
    CullHashResult r;
    /* 视口右边界恰好等于盒 0 左边界：重叠测试用的是闭边界，必须命中 */
    CullView v = {0, 0, 10, 10};
    CK(cull_hash_query(h, v, &r, &err) == CULL_OK);
    CK(r.count == 2);
    CK(r.ids[0] == 0 && r.ids[1] == 1);
    CK(same_as_brute(b, 3, v, &r));
    cull_hash_result_free(&r);
    cull_hash_free(h);
}

static void test_limits(void) {
    CullHashOpts o;
    CullHashError err;
    CullHash *h = NULL;

    /* 桶数超限：报 CULL_ERR_HASH_LIMIT，limit 与 message 都给出上限值 */
    CullBox big[4] = {{0,0,10,10,0},{1000,0,10,10,0},{0,1000,10,10,0},{1000,1000,10,10,0}};
    memset(&o, 0, sizeof o);
    o.cell_w = 1; o.cell_h = 1; o.max_buckets = 64;
    CK(cull_hash_build(big, 4, &o, &h, &err) == CULL_ERR_HASH_LIMIT);
    CK(h == NULL);
    CK(err.code == CULL_ERR_HASH_LIMIT);
    CK(err.limit == 64);
    CK(err.field && strcmp(err.field, "buckets") == 0);
    CK(strstr(err.message, "64") != NULL);

    /* 插入数超限：同样报错并给出上限。单元较细、桶数上限放宽，让插入先触发。 */
    CullBox span[1] = {{0, 0, 200, 200, 0}};
    memset(&o, 0, sizeof o);
    o.cell_w = 1; o.cell_h = 1; o.max_buckets = 100000; o.max_entries = 100;
    CK(cull_hash_build(span, 1, &o, &h, &err) == CULL_ERR_HASH_LIMIT);
    CK(err.limit == 100);
    CK(err.field && strcmp(err.field, "entries") == 0);
    CK(strstr(err.message, "100") != NULL);

    /* 默认上限可见并可被读取 */
    CK(CULL_HASH_MAX_BUCKETS > 0 && CULL_HASH_MAX_ENTRIES > 0);

    /* 极端范围 + 极细单元：不能溢出，仍报上限错误 */
    CullBox huge[1] = {{-1e12, -1e12, 2e12, 2e12, 0}};
    memset(&o, 0, sizeof o);
    o.cell_w = 1e-6; o.cell_h = 1e-6;
    CK(cull_hash_build(huge, 1, &o, &h, &err) == CULL_ERR_HASH_LIMIT);
    CK(err.limit == CULL_HASH_MAX_BUCKETS);
    CK(err.field && strcmp(err.field, "buckets") == 0);

    /* 非法参数 */
    memset(&o, 0, sizeof o);
    CK(cull_hash_build(NULL, 4, &o, &h, &err) == CULL_ERR_ARG);
    CK(cull_hash_build(big, -1, &o, &h, &err) == CULL_ERR_ARG);
    CullBox neg[1] = {{0,0,-5,10,0}};
    CK(cull_hash_build(neg, 1, &o, &h, &err) == CULL_ERR_HASH_BOX);
    CK(err.index == 0 && err.field && strcmp(err.field, "w") == 0);
    CullBox nanb[1] = {{0,0,NAN,10,0}};
    CK(cull_hash_build(nanb, 1, &o, &h, &err) == CULL_ERR_HASH_BOX);

    /* n = 0：合法，空索引可查询，输出为空 */
    CK(cull_hash_build(big, 0, &o, &h, &err) == CULL_OK);
    CullHashResult r;
    CullView v = {0, 0, 10, 10};
    CK(cull_hash_query(h, v, &r, &err) == CULL_OK);
    CK(r.count == 0 && r.candidates == 0);
    cull_hash_result_free(&r);
    cull_hash_free(h);

    /* 查询时视口非法 */
    memset(&o, 0, sizeof o);
    CK(cull_hash_build(big, 4, &o, &h, &err) == CULL_OK);
    CullView bad = {0, 0, 0, 10};
    CK(cull_hash_query(h, bad, &r, &err) == CULL_ERR_VIEW);
    CK(cull_hash_query(NULL, v, &r, &err) == CULL_ERR_ARG);
    cull_hash_free(h);
}

static void write_scratch(const char *path) {
    /* Helper used only to exercise the export path in tests. */
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

static int read_all(const char *path, char **buf, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc((size_t)n + 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return 0; }
    p[n] = '\0';
    fclose(f);
    *buf = p;
    *len = n;
    return 1;
}

static void test_export_and_reproducibility(void) {
    enum { N = 400 };
    CullBox *b = malloc(sizeof(CullBox) * N);
    CullView v = {0, 0, 300, 300};
    for (int i = 0; i < N; ++i) {
        b[i].x = frand(-200, 900);
        b[i].y = frand(-200, 900);
        b[i].w = frand(1, 80);
        b[i].h = frand(1, 80);
        b[i].layer = i % 5;
    }
    CullHashOpts o;
    memset(&o, 0, sizeof o);
    CullHashError err;
    CullHash *h1 = NULL, *h2 = NULL;
    CK(cull_hash_build(b, N, &o, &h1, &err) == CULL_OK);
    CK(cull_hash_build(b, N, &o, &h2, &err) == CULL_OK);

    const char *p1 = "build/hash_buckets_a.txt";
    const char *p2 = "build/hash_buckets_b.txt";
    write_scratch(p1); /* ensure build/ exists via Makefile rule */
    CK(cull_hash_export_buckets(h1, p1) == CULL_OK);
    CK(cull_hash_export_buckets(h2, p2) == CULL_OK);

    char *a = NULL, *c = NULL;
    long la = 0, lc = 0;
    CK(read_all(p1, &a, &la) == 1);
    CK(read_all(p2, &c, &lc) == 1);
    CK(la == lc && memcmp(a, c, (size_t)la) == 0); /* 两次构建逐字节可复现 */

    /* 导出的表头含桶数/冲突数/冲突率（两位小数） */
    CK(strstr(a, "buckets ") != NULL);
    CK(strstr(a, "collisions ") != NULL);
    CK(strstr(a, "collision_rate ") != NULL);
    char *rate = strstr(a, "collision_rate ");
    CK(rate != NULL);
    const char *num = rate + strlen("collision_rate ");
    double shown = atof(num);
    double want = round(shown * 100.0) / 100.0;
    CK(fabs(shown - want) < 1e-12);
    const char *nl = strchr(num, '\n');
    const char *dot = strchr(num, '.');
    CK(nl != NULL && dot != NULL && nl - dot == 3); /* exactly two decimals */

    /* 同一索引重复查询结果一致；桶内顺序升序 */
    CullHashResult r1, r2;
    CK(cull_hash_query(h1, v, &r1, &err) == CULL_OK);
    CK(cull_hash_query(h1, v, &r2, &err) == CULL_OK);
    CK(r1.count == r2.count && memcmp(r1.ids, r2.ids, sizeof(int) * (size_t)r1.count) == 0);
    CK(same_as_brute(b, N, v, &r1));
    cull_hash_result_free(&r1);
    cull_hash_result_free(&r2);

    /* 恰好覆盖：26 个手算采样视口全部与暴力一致，候选数不高于 N */
    for (int k = 0; k < 26; ++k) {
        CullView vv = {frand(-400, 900), frand(-400, 900), frand(1, 500), frand(1, 500)};
        CullHashResult rr;
        CK(cull_hash_query(h1, vv, &rr, &err) == CULL_OK);
        CK(same_as_brute(b, N, vv, &rr));
        CK(rr.candidates >= rr.count && rr.candidates <= N);
        cull_hash_result_free(&rr);
    }

    /* 桶表导出失败：路径不可写时返回 IO 错误 */
    CK(cull_hash_export_buckets(h1, "/nonexistent-dir/x/y.txt") == CULL_ERR_HASH_IO);

    free(a); free(c);
    cull_hash_free(h1);
    cull_hash_free(h2);
    free(b);
}

static void test_randomized_equivalence(void) {
    /* 随机场景 + 随机视口：哈希路径必须与逐对象比较逐项相同。 */
    for (int trial = 0; trial < 12; ++trial) {
        int n = (int)(rnd() % 500) + 1;
        CullBox *b = malloc(sizeof(CullBox) * (size_t)n);
        for (int i = 0; i < n; ++i) {
            b[i].x = frand(-1000, 1000);
            b[i].y = frand(-1000, 1000);
            b[i].w = frand(0, 120);
            b[i].h = frand(0, 120);
            b[i].layer = i % 7;
        }
        CullHashOpts o;
        memset(&o, 0, sizeof o);
        CullHashError err;
        CullHash *h = NULL;
        CK(cull_hash_build(b, n, &o, &h, &err) == CULL_OK);
        for (int q = 0; q < 40; ++q) {
            CullView v = {frand(-1500, 1500), frand(-1500, 1500),
                          frand(0.001, 900), frand(0.001, 900)};
            CullHashResult r;
            CK(cull_hash_query(h, v, &r, &err) == CULL_OK);
            CK(same_as_brute(b, n, v, &r));
            cull_hash_result_free(&r);
        }
        cull_hash_free(h);
        free(b);
    }
}

int main(void) {
    test_legacy_unchanged();
    test_two_stage_and_stats();
    test_boundaries();
    test_limits();
    test_export_and_reproducibility();
    test_randomized_equivalence();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
