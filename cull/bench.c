#define _POSIX_C_SOURCE 199309L
#include "cull.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static unsigned long long rng_state = 0x9e3779b97f4a7c15ull;
static unsigned long long rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

static int bench(int n) {
    int bad = 0;
    CullBox *b = (CullBox*)malloc(sizeof(CullBox) * (size_t)n);
    int *perm = (int*)malloc(sizeof(int) * (size_t)n);
    if (!b || !perm) return 1;
    for (int i = 0; i < n; ++i) perm[i] = i;
    for (int i = n - 1; i > 0; --i) { /* 打乱插入序号 */
        int j = (int)(rnd() % (unsigned long long)(i + 1));
        int t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (int i = 0; i < n; ++i) {
        b[i].x = (double)(rnd() % 2000) - 500.0;
        b[i].y = (double)(rnd() % 2000) - 500.0;
        b[i].w = 20.0 + (double)(rnd() % 30);
        b[i].h = 20.0 + (double)(rnd() % 30);
        b[i].layer = (int)(rnd() % 8);
        b[i].seq = perm[i];
    }
    CullView v = {0, 0, 1000, 1000};
    CullStatus st;
    CullLayered L, P;
    enum { REPS = 7 };
    double tL = 1e30, tP = 1e30;

    for (int r = 0; r < REPS; ++r) {
        double t0 = now_s();
        if (cull_run_layered(b, n, v, 1, &L, &st) != CULL_OK) { printf("layered err %d\n", st.code); return 1; }
        double t1 = now_s();
        if (t1 - t0 < tL) tL = t1 - t0;
        if (r < REPS - 1) cull_layered_free(&L);
    }
    for (int r = 0; r < REPS; ++r) {
        double t0 = now_s();
        if (cull_run_layered(b, n, v, 0, &P, &st) != CULL_OK) { printf("plain err %d\n", st.code); return 1; }
        double t1 = now_s();
        if (t1 - t0 < tP) tP = t1 - t0;
        if (r < REPS - 1) cull_layered_free(&P);
    }

    /* 顺序差异次数: 分层输出与插入序输出逐位置比较 */
    long order_diff = 0;
    if (L.count != P.count) { printf("FAIL count %d != %d\n", L.count, P.count); ++bad; }
    for (int k = 0; k < L.count && k < P.count; ++k)
        if (L.ids[k] != P.ids[k]) ++order_diff;

    /* 逐项核对 1: 关闭分层的输出与 cull_run 完全一致 */
    CullResult base;
    if (cull_run(b, n, v, &base) != 0) return 1;
    if (base.count != P.count) { printf("FAIL base count\n"); ++bad; }
    for (int k = 0; k < base.count && k < P.count; ++k)
        if (base.ids[k] != P.ids[k]) { printf("FAIL plain!=cull_run at %d\n", k); ++bad; break; }

    /* 逐项核对 2: 关闭分层的输出严格按插入序(下标递增) */
    for (int k = 1; k < P.count; ++k)
        if (P.ids[k] <= P.ids[k - 1]) { printf("FAIL plain not insertion order at %d\n", k); ++bad; break; }

    /* 逐项核对 3: 分层输出与插入序输出是同一集合(排序后逐项相同) */
    {
        int *sorted = (int*)malloc(sizeof(int) * (size_t)L.count);
        for (int k = 0; k < L.count; ++k) sorted[k] = L.ids[k];
        qsort(sorted, (size_t)L.count, sizeof(int), cmp_int);
        for (int k = 0; k < L.count; ++k)
            if (sorted[k] != P.ids[k]) { printf("FAIL set mismatch at %d\n", k); ++bad; break; }
        free(sorted);
    }

    /* 逐项核对 4: 分层输出按 (layer, seq) 不降序 */
    for (int k = 1; k < L.count; ++k) {
        int a = L.ids[k - 1], c = L.ids[k];
        if (b[a].layer > b[c].layer ||
            (b[a].layer == b[c].layer && b[a].seq > b[c].seq)) {
            printf("FAIL layered order at %d\n", k); ++bad; break;
        }
    }

    /* 逐项核对 5: 每桶 visible/clipped 与逐盒重算一致 */
    {
        long sumv = 0;
        for (int j = 0; j < L.bucket_count; ++j) {
            int ev = 0, ec = 0;
            for (int i = 0; i < n; ++i)
                if (b[i].layer == L.buckets[j].layer) {
                    CullView vv = v;
                    if (!(b[i].x + b[i].w < vv.x || b[i].x > vv.x + vv.w ||
                          b[i].y + b[i].h < vv.y || b[i].y > vv.y + vv.h)) ++ev;
                    else ++ec;
                }
            if (L.buckets[j].visible != ev || L.buckets[j].clipped != ec) {
                printf("FAIL bucket %d stats\n", j); ++bad;
            }
            sumv += L.buckets[j].visible;
        }
        if (sumv != L.count) { printf("FAIL bucket visible sum\n"); ++bad; }
    }

    printf("n=%d visible=%d buckets=%d\n", n, L.count, L.bucket_count);
    printf("  顺序差异次数 %ld (共 %d 个可见)\n", order_diff, L.count);
    printf("  比较次数: 分层 %llu, 关闭 %llu\n", L.comparisons, P.comparisons);
    printf("  耗时(7 次取最优): 分层 %.3f ms, 关闭 %.3f ms\n", tL * 1e3, tP * 1e3);
    printf("  逐项核对: %s\n", bad ? "FAIL" : "PASS");

    cull_layered_free(&L);
    cull_layered_free(&P);
    free(base.ids);
    free(b); free(perm);
    return bad ? 1 : 0;
}

int main(void) {
    int rc = 0;
    rc |= bench(10000);
    rc |= bench(100000);
    return rc;
}
