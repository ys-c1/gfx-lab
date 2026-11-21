/*
 * bench.c —— 两段式层采样的规模、误差、耗时与缓存统计
 *
 * 两档规模：1000 与 10000 次批量采样，每次 8 层、同一位置。
 *
 * 参考实现（都在本文件内独立重写，不调用 mip 模块的选层/采样逻辑）：
 *   - 盒式金字塔 ref_build()，逐层与 MipChain 比对字节；
 *   - 点采样公式 ref_sample()，逐字节核对 mip_sample 与两段式输出；
 *   - 层表（尺寸 / 字节数 / FNV-1a 校验和）逐项核对，并导出 CSV。
 *
 * 耗时拆成四段，同一份输入、每段重复多轮取最小值：
 *   select  只做第一段选层（每批 8 次）
 *   sample  只做第二段：预先选好的 8 层，对同一位置批量采样
 *   batch   完整两段（选层 + 批量采样）
 *   legacy  旧接口 mip_sample 一张一张采样
 */
#define _POSIX_C_SOURCE 199309L
#include "mip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IMG_W 512
#define IMG_H 512
#define BATCH_LAYERS 8
#define ROUNDS 5

/* ---------------- 随机数（固定种子，可重复） ---------------- */

static unsigned long long rng_state;
static void rng_reset(void) { rng_state = 0x9E3779B97F4A7C15ULL; }
static unsigned rnd_u32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned)(rng_state >> 32);
}
static double rnd_unit(void) { return (rnd_u32() >> 8) * (1.0 / 16777216.0); }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---------------- 源图 ---------------- */

static void make_source(unsigned char *src, int w, int h) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char *p = src + ((size_t)y * w + x) * 3;
            p[0] = (unsigned char)((x * 255) / (w - 1));
            p[1] = (unsigned char)((y * 255) / (h - 1));
            p[2] = (unsigned char)(((x / 8) ^ (y / 8)) & 1 ? 230 : 25);
        }
}

/* ---------------- 参考实现：独立重造金字塔 ---------------- */

static int ref_levels(int w, int h) {
    int n = 1;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; ++n; }
    return n;
}

static unsigned char **ref_build(const unsigned char *src, int w, int h, int *out_levels) {
    int levels = ref_levels(w, h);
    unsigned char **ref = (unsigned char**)calloc((size_t)levels, sizeof(unsigned char*));
    if (!ref) return NULL;
    ref[0] = (unsigned char*)malloc((size_t)w * h * 3);
    if (!ref[0]) { free(ref); return NULL; }
    memcpy(ref[0], src, (size_t)w * h * 3);
    int cw = w, ch = h;
    for (int l = 1; l < levels; ++l) {
        int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
        ref[l] = (unsigned char*)malloc((size_t)nw * nh * 3);
        if (!ref[l]) {
            for (int k = 0; k < l; ++k) free(ref[k]);
            free(ref);
            return NULL;
        }
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 3; ++c) {
                    int x0 = 2 * x, x1 = 2 * x + 1, y0 = 2 * y, y1 = 2 * y + 1;
                    if (x0 >= cw) x0 = cw - 1;
                    if (x1 >= cw) x1 = cw - 1;
                    if (y0 >= ch) y0 = ch - 1;
                    if (y1 >= ch) y1 = ch - 1;
                    unsigned s = ref[l-1][((size_t)y0 * cw + x0) * 3 + c]
                               + ref[l-1][((size_t)y0 * cw + x1) * 3 + c]
                               + ref[l-1][((size_t)y1 * cw + x0) * 3 + c]
                               + ref[l-1][((size_t)y1 * cw + x1) * 3 + c];
                    ref[l][((size_t)y * nw + x) * 3 + c] = (unsigned char)(s / 4);
                }
        cw = nw; ch = nh;
    }
    *out_levels = levels;
    return ref;
}

static void ref_free(unsigned char **ref, int levels) {
    if (!ref) return;
    for (int l = 0; l < levels; ++l) free(ref[l]);
    free(ref);
}

static void ref_sample(const unsigned char *layer, int w, int h, double u, double v,
                       unsigned char out[3]) {
    long x = (long)(u * w);
    long y = (long)(v * h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > w - 1) x = w - 1;
    if (y > h - 1) y = h - 1;
    const unsigned char *p = layer + ((size_t)y * (size_t)w + (size_t)x) * 3;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
}

/* ---------------- 一档规模的测量 ---------------- */

typedef struct {
    const char *name;
    long batches;
    int layers_per_batch;
    long samples;
    long hits;
    long misses;
    long mismatched_samples;
    long mismatched_bytes;
    int max_abs_err;
    double select_ms;
    double sample_ms;
    double batch_ms;
    double legacy_ms;
    /* 摊薄场景：选层一次，同一组层在 batches 个位置上各采一次 */
    double tile_ms;
    double tile_legacy_ms;
    long tile_mismatched;
} BenchRow;

static void run_scale(BenchRow *row, const char *name, long batches,
                      const MipChain *c, unsigned char **ref, MipLayerCache *cache,
                      const int *level_pool, int pool_size) {
    const int K = BATCH_LAYERS;
    memset(row, 0, sizeof *row);
    row->name = name;
    row->batches = batches;
    row->layers_per_batch = K;

    /* 固定输入：每批一组层号 + 一个位置；四段计时都跑同一份。 */
    int *levels = (int*)malloc((size_t)batches * (size_t)K * sizeof(int));
    double *uv = (double*)malloc((size_t)batches * 2 * sizeof(double));
    unsigned char *out = (unsigned char*)malloc((size_t)K * 3);
    unsigned char *want = (unsigned char*)malloc((size_t)K * 3);
    MipLayer layer_set[BATCH_LAYERS];
    if (!levels || !uv || !out || !want) {
        free(levels); free(uv); free(out); free(want);
        fprintf(stderr, "bench: out of memory\n");
        exit(1);
    }
    rng_reset();
    for (long b = 0; b < batches; ++b) {
        /* 批量内层号不重复；跨批次循环复用同一层号池。 */
        for (int k = 0; k < K; ++k)
            levels[(size_t)b * K + k] = level_pool[(size_t)(b + k) % (size_t)pool_size];
        uv[b*2+0] = rnd_unit();
        uv[b*2+1] = rnd_unit();
    }

    /* ---- 第一段单独计时：只选层，不采样 ---- */
    long hits = 0, misses = 0;
    double best = 1e300;
    for (int r = 0; r < ROUNDS; ++r) {
        mip_cache_free(cache);
        if (mip_cache_init(cache, c, NULL) != MIP_OK) exit(1);
        double t0 = now_ms();
        for (long b = 0; b < batches; ++b)
            if (mip_layer_select_list(cache, levels + (size_t)b * K, K, layer_set, NULL) != MIP_OK)
                exit(1);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
        MipStats st;
        mip_cache_stats(cache, &st);
        hits = st.hits;
        misses = st.misses;
    }
    row->select_ms = best;
    row->hits = hits;
    row->misses = misses;
    row->samples = (long)batches * K;

    /* ---- 第二段单独计时：层已选好，只做同位置批量采样 ---- */
    if (mip_layer_select_list(cache, levels, K, layer_set, NULL) != MIP_OK) exit(1);
    best = 1e300;
    for (int r = 0; r < ROUNDS; ++r) {
        double t0 = now_ms();
        for (long b = 0; b < batches; ++b)
            if (mip_layer_sample_same(layer_set, K, uv[b*2+0], uv[b*2+1], out, NULL) != MIP_OK)
                exit(1);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    row->sample_ms = best;

    /* ---- 两段完整路径计时（冷缓存，命中统计来自这里） ---- */
    best = 1e300;
    for (int r = 0; r < ROUNDS; ++r) {
        mip_cache_free(cache);
        if (mip_cache_init(cache, c, NULL) != MIP_OK) exit(1);
        double t0 = now_ms();
        for (long b = 0; b < batches; ++b)
            if (mip_batch_sample(cache, levels + (size_t)b * K, K,
                                 uv[b*2+0], uv[b*2+1], out, NULL, NULL) != MIP_OK)
                exit(1);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    row->batch_ms = best;

    /* ---- 旧接口：一张一张采样计时 ---- */
    best = 1e300;
    for (int r = 0; r < ROUNDS; ++r) {
        double t0 = now_ms();
        for (long b = 0; b < batches; ++b)
            for (int k = 0; k < K; ++k)
                if (mip_sample(c, levels[(size_t)b * K + k],
                               uv[b*2+0], uv[b*2+1], want + (size_t)k * 3) != 0)
                    exit(1);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    row->legacy_ms = best;

    /* ---- 核对：两段式 vs 旧接口，两者 vs 本文件内的独立公式 ---- */
    long mism_samples = 0, mism_bytes = 0;
    int max_err = 0;
    for (long b = 0; b < batches; ++b) {
        if (mip_batch_sample(cache, levels + (size_t)b * K, K,
                             uv[b*2+0], uv[b*2+1], out, NULL, NULL) != MIP_OK) {
            fprintf(stderr, "bench: verify batch %ld failed\n", b);
            exit(1);
        }
        for (int k = 0; k < K; ++k) {
            int l = levels[(size_t)b * K + k];
            if (mip_sample(c, l, uv[b*2+0], uv[b*2+1], want + (size_t)k * 3) != 0) exit(1);
            if (memcmp(out + (size_t)k * 3, want + (size_t)k * 3, 3) != 0) ++mism_samples;
            unsigned char indep[3];
            ref_sample(ref[l], c->w[l], c->h[l], uv[b*2+0], uv[b*2+1], indep);
            for (int ch = 0; ch < 3; ++ch) {
                int d = abs((int)out[(size_t)k*3+ch] - (int)want[(size_t)k*3+ch]);
                if (d > max_err) max_err = d;
                d = abs((int)want[(size_t)k*3+ch] - (int)indep[ch]);
                if (d) ++mism_bytes;
                if (d > max_err) max_err = d;
            }
        }
    }
    row->mismatched_samples = mism_samples;
    row->mismatched_bytes = mism_bytes;
    row->max_abs_err = max_err;

    /* ---- 摊薄场景：选一次层，在 batches 个位置上各批量采一次 ----
     * 这是两段式设计的目标用法：层选择固定，位置每像素变。 */
    best = 1e300;
    for (int r = 0; r < ROUNDS; ++r) {
        if (mip_layer_select_list(cache, levels, K, layer_set, NULL) != MIP_OK) exit(1);
        double t0 = now_ms();
        for (long b = 0; b < batches; ++b)
            if (mip_layer_sample_same(layer_set, K, uv[b*2+0], uv[b*2+1], out, NULL) != MIP_OK)
                exit(1);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    row->tile_ms = best;

    best = 1e300;
    for (int r = 0; r < ROUNDS; ++r) {
        double t0 = now_ms();
        for (long b = 0; b < batches; ++b)
            for (int k = 0; k < K; ++k)
                if (mip_sample(c, levels[k], uv[b*2+0], uv[b*2+1], want + (size_t)k * 3) != 0)
                    exit(1);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
    }
    row->tile_legacy_ms = best;

    long tile_mismatch = 0;
    for (long b = 0; b < batches; ++b) {
        if (mip_layer_sample_same(layer_set, K, uv[b*2+0], uv[b*2+1], out, NULL) != MIP_OK)
            exit(1);
        for (int k = 0; k < K; ++k) {
            mip_sample(c, levels[k], uv[b*2+0], uv[b*2+1], want + (size_t)k * 3);
            if (memcmp(out + (size_t)k * 3, want + (size_t)k * 3, 3) != 0) ++tile_mismatch;
        }
    }
    row->tile_mismatched = tile_mismatch;

    free(levels); free(uv); free(out); free(want);
}

/* ---------------- 层表核对 ---------------- */

static int check_layer_table(const MipChain *c, unsigned char **ref, int ref_level_count,
                             MipLayerTable *t) {
    if (t->count != ref_level_count) {
        printf("layer table: level count mismatch (table %d, reference %d)\n",
               t->count, ref_level_count);
        return 1;
    }
    int bad = 0;
    for (int l = 0; l < t->count; ++l) {
        size_t bytes = (size_t)c->w[l] * (size_t)c->h[l] * 3;
        unsigned int ref_sum = mip_checksum(ref[l], bytes);
        int size_ok = t->layers[l].level == l &&
                      t->layers[l].w == c->w[l] && t->layers[l].h == c->h[l] &&
                      t->layers[l].bytes == bytes;
        int sum_ok = t->checksum[l] == ref_sum;
        int bytes_ok = memcmp(t->layers[l].pix, ref[l], bytes) == 0;
        if (!size_ok || !sum_ok || !bytes_ok) {
            ++bad;
            printf("layer table: level %d mismatch (size %s, checksum %s, bytes %s)\n",
                   l, size_ok ? "ok" : "BAD", sum_ok ? "ok" : "BAD", bytes_ok ? "ok" : "BAD");
        }
    }
    if (!bad)
        printf("layer table: %d levels match reference (size, bytes, checksum)\n", t->count);
    return bad;
}

int main(void) {
    unsigned char *src = (unsigned char*)malloc((size_t)IMG_W * IMG_H * 3);
    if (!src) return 1;
    make_source(src, IMG_W, IMG_H);

    MipChain c;
    if (mip_build(&c, src, IMG_W, IMG_H) != 0) { free(src); return 1; }
    printf("image %dx%d, levels %d (%dx%d .. %dx%d), %d layers per batch\n",
           IMG_W, IMG_H, c.levels, c.w[0], c.h[0], c.w[c.levels-1], c.h[c.levels-1],
           BATCH_LAYERS);

    int ref_levels_n = 0;
    unsigned char **ref = ref_build(src, IMG_W, IMG_H, &ref_levels_n);
    if (!ref) { mip_free(&c); free(src); return 1; }

    /* 1. 层表导出并逐项核对 */
    MipLayerTable table;
    MipError err;
    int bad = 0;
    if (mip_layer_table_build(&c, &table, &err) != MIP_OK) {
        printf("layer table: build failed (field %s)\n", err.field ? err.field : "?");
        bad = 1;
    } else {
        bad = check_layer_table(&c, ref, ref_levels_n, &table);
        if (mip_layer_table_write_csv(&table, "bench_layers.csv", &err) == MIP_OK)
            printf("layer table: wrote bench_layers.csv (%d rows)\n", table.count);
        else {
            printf("layer table: CSV write failed (field %s)\n", err.field ? err.field : "?");
            bad = 1;
        }
        mip_layer_table_free(&table);
    }
    if (bad) {
        ref_free(ref, ref_levels_n);
        mip_free(&c);
        free(src);
        return 1;
    }

    /* 2. 两档规模 */
    MipLayerCache cache;
    if (mip_cache_init(&cache, &c, &err) != MIP_OK) {
        printf("cache init failed (field %s)\n", err.field ? err.field : "?");
        ref_free(ref, ref_levels_n); mip_free(&c); free(src);
        return 1;
    }

    /* 层号池：全部层，再补几个高频层（0 / 中层 / 最末层）。 */
    int pool[40];
    int pool_size = 0;
    for (int l = 0; l < c.levels; ++l) pool[pool_size++] = l;
    pool[pool_size++] = 0;
    pool[pool_size++] = c.levels / 2;
    pool[pool_size++] = c.levels - 1;

    BenchRow rows[2];
    run_scale(&rows[0], "1k", 1000, &c, ref, &cache, pool, pool_size);
    run_scale(&rows[1], "10k", 10000, &c, ref, &cache, pool, pool_size);

    printf("\n");
    printf("+-------+--------+--------+--------+--------+---------+---------+---------+---------+---------+-----------+--------+\n");
    printf("| scale | levels | layers | samples| hits   | misses  | select  | sample  | batch   | legacy  | mismatch  | max    |\n");
    printf("|       |        | /batch |        |        |         | ms      | ms      | ms      | ms      | samples   | err    |\n");
    printf("+-------+--------+--------+--------+--------+---------+---------+---------+---------+---------+-----------+--------+\n");
    for (int i = 0; i < 2; ++i) {
        const BenchRow *r = &rows[i];
        long hit_x10 = r->samples ? (r->hits * 1000) / r->samples : 0;
        printf("| %5s | %6d | %6d | %6ld | %4ld.%1ld%% | %7ld | %7.3f | %7.3f | %7.3f | %7.3f | %9ld | %6d |\n",
               r->name, c.levels, r->layers_per_batch, r->samples,
               hit_x10 / 10, hit_x10 % 10, r->misses,
               r->select_ms, r->sample_ms, r->batch_ms, r->legacy_ms,
               r->mismatched_samples, r->max_abs_err);
    }
    printf("+-------+--------+--------+--------+--------+---------+---------+---------+---------+---------+-----------+--------+\n");
    printf("select/sample/batch/legacy: best of %d rounds on identical inputs.\n", ROUNDS);
    printf("mismatch counts two-stage bytes against mip_sample; max err also checks both against\n");
    printf("the independent sampler in this file. legacy = mip_sample one layer at a time.\n");

    printf("\nPer-pixel batches (select once per batch):\n");
    printf("+-------+---------+---------+---------+-----------+--------+\n");
    printf("| scale | batch   | legacy  | speedup | mismatch  | max    |\n");
    printf("|       | ms      | ms      |         | samples   | err    |\n");
    printf("+-------+---------+---------+---------+-----------+--------+\n");
    for (int i = 0; i < 2; ++i) {
        const BenchRow *r = &rows[i];
        printf("| %5s | %7.3f | %7.3f | %6.2fx | %9ld | %6d |\n",
               r->name, r->batch_ms, r->legacy_ms,
               r->batch_ms > 0.0 ? r->legacy_ms / r->batch_ms : 0.0,
               r->mismatched_samples, r->max_abs_err);
    }
    printf("+-------+---------+---------+---------+-----------+--------+\n");

    printf("\nSelect once, reuse across positions (the two-stage win):\n");
    printf("+-------+---------+---------+---------+-----------+--------+\n");
    printf("| scale | tiled   | legacy  | speedup | mismatch  |\n");
    printf("|       | ms      | ms      |         | samples   |\n");
    printf("+-------+---------+---------+---------+-----------+--------+\n");
    for (int i = 0; i < 2; ++i) {
        const BenchRow *r = &rows[i];
        printf("| %5s | %7.3f | %7.3f | %6.2fx | %9ld |\n",
               r->name, r->tile_ms, r->tile_legacy_ms,
               r->tile_ms > 0.0 ? r->tile_legacy_ms / r->tile_ms : 0.0,
               r->tile_mismatched);
    }
    printf("+-------+---------+---------+---------+-----------+--------+\n");

    /* 3. 关闭缓存：结果不变，命中全部转为未命中 */
    mip_cache_free(&cache);
    if (mip_cache_init(&cache, &c, NULL) != MIP_OK) return 1;
    mip_cache_set_enabled(&cache, 0);
    long mismatch_off = 0;
    int off_levels[BATCH_LAYERS];
    for (int k = 0; k < BATCH_LAYERS; ++k) off_levels[k] = pool[k];
    rng_reset();
    for (long b = 0; b < 1000; ++b) {
        for (int k = 0; k < BATCH_LAYERS; ++k)
            off_levels[k] = pool[(size_t)(b + k) % (size_t)pool_size];
        double u = rnd_unit(), v = rnd_unit();
        unsigned char a[3*BATCH_LAYERS], r[3*BATCH_LAYERS];
        if (mip_batch_sample(&cache, off_levels, BATCH_LAYERS, u, v, a, NULL, NULL) != MIP_OK) {
            mismatch_off = -1;
            break;
        }
        for (int k = 0; k < BATCH_LAYERS; ++k) {
            mip_sample(&c, off_levels[k], u, v, r + (size_t)k * 3);
            if (memcmp(a + (size_t)k * 3, r + (size_t)k * 3, 3) != 0) ++mismatch_off;
        }
    }
    MipStats off_stats;
    mip_cache_stats(&cache, &off_stats);
    printf("\ncache off: %ld mismatches vs mip_sample over 1000 batches; hits %ld, misses %ld\n",
           mismatch_off, off_stats.hits, off_stats.misses);

    int ok = bad == 0 && mismatch_off == 0 &&
             rows[0].mismatched_samples == 0 && rows[1].mismatched_samples == 0 &&
             rows[0].mismatched_bytes == 0 && rows[1].mismatched_bytes == 0 &&
             rows[0].max_abs_err == 0 && rows[1].max_abs_err == 0 &&
             rows[0].tile_mismatched == 0 && rows[1].tile_mismatched == 0;
    printf("result: %s\n", ok ? "PASS (bit-exact vs reference)" : "FAIL");

    mip_cache_free(&cache);
    ref_free(ref, ref_levels_n);
    mip_free(&c);
    free(src);
    return ok ? 0 : 1;
}
