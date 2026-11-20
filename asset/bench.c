#define _POSIX_C_SOURCE 199309L
#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * First-screen load benchmark.
 *
 * Two package sizes (100 and 1000 entries) with the same 1/10 first-screen
 * ratio.  Each entry is a synthetic texture page built from a small palette
 * so pages are compressible but not identical.
 *
 * Reported per size:
 *   pack bytes        on-disk size of the chunked/compressed package
 *   raw bytes         size of the logical uncompressed stream
 *   ratio             raw / pack
 *   first-screen      bytes inflated and blocks inflated when entering scene
 *   full              bytes inflated and blocks inflated by whole-package load
 *   time              median wall time of the scene-entry load, and of the
 *                     whole-package load as the control
 *   err               max |byte difference| after a full round trip; a lossless
 *                     codec must report 0
 *
 * Compressed and uncompressed packages are unpacked and compared too, so the
 * "compression off is byte-identical" guarantee is measured on every run.
 */

#define PAGE_SIZE 4096
#define ROUNDS 21

static unsigned char page_template[PAGE_SIZE];
/* A page is two repeated halves plus a sparse noise band: classic texture
 * data with low entropy locally and high entropy in patches. */
static void make_page(unsigned char *dst, int id) {
    unsigned int seed = (unsigned int)(id * 2654435761u + 1u);
    if (seed == 0) seed = 1;
    for (int i = 0; i < PAGE_SIZE; ++i) dst[i] = page_template[i];
    for (int i = 0; i < 256; ++i) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        int at = (int)(seed % (PAGE_SIZE - 32));
        for (int j = 0; j < 32; ++j) dst[at + j] = (unsigned char)(seed >> (j & 7));
    }
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static double median_ms(const char *path, const ApIndex *idx, int first_screen,
                        unsigned int *loaded_size, int *loaded_blocks) {
    double t[ROUNDS];
    for (int r = 0; r < ROUNDS; ++r) {
        unsigned char *buf = NULL;
        unsigned int sz = 0;
        int blocks = 0;
        double t0 = now_ms();
        int rc = first_screen
            ? ap_load_first_screen(path, idx, &buf, &sz, &blocks)
            : ap_load_all(path, idx, &buf, &sz, &blocks);
        double t1 = now_ms();
        if (rc != 0) { printf("load failed: %d\n", rc); return -1.0; }
        free(buf);
        t[r] = t1 - t0;
        if (loaded_size) *loaded_size = sz;
        if (loaded_blocks) *loaded_blocks = blocks;
    }
    qsort(t, ROUNDS, sizeof(double), cmp_double);
    return t[ROUNDS / 2];
}

static int run_case(int n, unsigned int chunk_size, int first_screen_n) {
    unsigned char *raw = (unsigned char*)malloc((size_t)n * PAGE_SIZE);
    ApItem *items = (ApItem*)calloc((size_t)n, sizeof(ApItem));
    if (!raw || !items) { free(raw); free(items); return 1; }
    for (int i = 0; i < n; ++i) {
        make_page(raw + (size_t)i * PAGE_SIZE, i);
        snprintf(items[i].name, sizeof(items[i].name), "page%04d.tex", i);
        items[i].size = PAGE_SIZE;
        items[i].data = raw + (size_t)i * PAGE_SIZE;
        items[i].first_screen = i < first_screen_n;
    }

    char cpath[64], upath[64];
    snprintf(cpath, sizeof(cpath), "build/bench_%d_c.gpak", n);
    snprintf(upath, sizeof(upath), "build/bench_%d_u.gpak", n);
    int err = -1;
    double tp0 = now_ms();
    int rc = ap_pack(cpath, items, n, chunk_size, 1, &err);
    double tp1 = now_ms();
    if (rc != 0) {
        printf("n=%d pack failed: %d block %d\n", n, rc, err);
        free(raw); free(items);
        return 1;
    }
    rc = ap_pack(upath, items, n, chunk_size, 0, NULL);
    if (rc != 0) { printf("n=%d raw pack failed: %d\n", n, rc); free(raw); free(items); return 1; }

    ApIndex cidx, uidx;
    if (ap_read(cpath, &cidx) != 0 || ap_read(upath, &uidx) != 0) {
        printf("n=%d read failed\n", n); free(raw); free(items); return 1;
    }

    unsigned int first_sz = 0, full_sz = 0;
    int first_blocks = 0, full_blocks = 0;
    double t_first = median_ms(cpath, &cidx, 1, &first_sz, &first_blocks);
    double t_full = median_ms(cpath, &cidx, 0, &full_sz, &full_blocks);
    double t_legacy = median_ms(upath, &uidx, 0, NULL, NULL);

    /* Byte-exact round trip: compressed and uncompressed must both match raw. */
    unsigned char *all = NULL;
    unsigned int all_sz = 0;
    unsigned int maxdiff = 0;
    if (ap_load_all(cpath, &cidx, &all, &all_sz, NULL) != 0) { printf("load_all failed\n"); return 1; }
    for (int i = 0; i < n; ++i) {
        unsigned char *page = all + cidx.entries[i].offset;
        for (int j = 0; j < PAGE_SIZE; ++j) {
            int d = (int)page[j] - (int)raw[(size_t)i * PAGE_SIZE + j];
            unsigned int mag = (unsigned int)(d < 0 ? -d : d);
            if (mag > maxdiff) maxdiff = mag;
        }
    }
    /* Entry-level extraction must agree with the bulk stream. */
    for (int i = 0; i < n; i += n / 10 ? n / 10 : 1) {
        unsigned char *one = NULL; unsigned int one_sz = 0;
        if (ap_extract(cpath, items[i].name, &one, &one_sz) != 0 ||
            one_sz != PAGE_SIZE || memcmp(one, raw + (size_t)i * PAGE_SIZE, PAGE_SIZE) != 0) {
            printf("n=%d extract mismatch on %s\n", n, items[i].name);
            maxdiff = 255;
        }
        free(one);
    }
    unsigned char *un = NULL;
    unsigned int un_sz = 0;
    if (ap_load_all(upath, &uidx, &un, &un_sz, NULL) != 0 || un_sz != (unsigned int)n * PAGE_SIZE ||
        memcmp(un, raw, (size_t)n * PAGE_SIZE) != 0) {
        printf("n=%d uncompressed round trip mismatch\n", n);
        maxdiff = 255;
    }
    free(un);

    FILE *f = fopen(cpath, "rb");
    fseek(f, 0, SEEK_END);
    long pack = ftell(f);
    fclose(f);

    printf("n=%d chunk=%u entries/page=%d bytes\n", n, chunk_size, PAGE_SIZE);
    printf("  pack       %8ld bytes in %d blocks (%.2f ms)\n", pack, cidx.block_count, tp1 - tp0);
    printf("  raw        %8u bytes\n", cidx.data_size);
    printf("  ratio      %8.3f x  (%.1f%% of raw)\n",
           (double)cidx.data_size / (double)pack, 100.0 * (double)pack / (double)cidx.data_size);
    printf("  first      %8u bytes, %d/%d blocks, %7.3f ms  (scene entry)\n",
           first_sz, first_blocks, cidx.block_count, t_first);
    printf("  full       %8u bytes, %d/%d blocks, %7.3f ms  (control)\n",
           full_sz, full_blocks, cidx.block_count, t_full);
    printf("  v1 raw     %8u bytes, %7.3f ms  (I/O floor, no decompression)\n",
           uidx.data_size, t_legacy);
    printf("  first/full %8.2f x fewer bytes inflated, %.2f x less time\n",
           full_sz ? (double)full_sz / (double)first_sz : 0.0,
           t_first > 0 ? t_full / t_first : 0.0);
    printf("  err        %8u  (max byte delta, must be 0)\n\n", maxdiff);

    free(all);
    free(raw);
    free(items);
    ap_free(&cidx);
    ap_free(&uidx);
    return maxdiff != 0;
}

int main(void) {
    for (int i = 0; i < PAGE_SIZE; ++i) page_template[i] = (unsigned char)((i % 61) * 3 + (i / 61) % 5);
    unsigned int chunk = 16u * 1024u;
    int bad = 0;
    bad |= run_case(100, chunk, 10);
    bad |= run_case(1000, chunk, 100);
    return bad ? 1 : 0;
}
