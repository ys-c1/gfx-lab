/*
 * Benchmark for the three-stage atlas packer.
 *
 * For strip widths 128 and 512 it reports, per configuration:
 *   - occupancy of the packed atlas and the error vs. a hand-packed reference
 *   - number of shelves (strips)
 *   - average packing time over many repetitions
 *
 * The "manual" reference is a literal hand-written shelf loop that places the
 * rects one at a time in input order; every result is also checked by an
 * independent geometric validator (bounds, overlap, area).
 */
#define _POSIX_C_SOURCE 199309L
#include "atlas.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NRECTS 60
#define REPS   2000

static unsigned lcg_state;
static unsigned lcg(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state;
}

/* Hand placement, block by block, in plain input order (no sort, no rotate). */
static int manual_pack(const AtlasRect *r, int n, int width, AtlasPos *p,
                       AtlasStats *s) {
    int x = 0, y = 0, shelf_h = 0, shelves = 1, used = 0;
    for (int i = 0; i < n; ++i) {
        if (r[i].w <= 0 || r[i].h <= 0 || r[i].w > width) return -2;
        if (x + r[i].w > width) { y += shelf_h; x = 0; shelf_h = 0; ++shelves; }
        p[i].x = x; p[i].y = y; p[i].placed = 1; p[i].rotated = 0;
        x += r[i].w;
        if (r[i].h > shelf_h) shelf_h = r[i].h;
        used += r[i].w * r[i].h;
    }
    s->width = width; s->used_px = used; s->shelves = shelves;
    s->height = y + shelf_h;
    return 0;
}

/* Independent geometric check; returns 0 when the layout is sound. */
static int validate(const AtlasRect *r, int n, int width, const AtlasPos *p,
                    const AtlasStats *s) {
    int area = 0;
    for (int i = 0; i < n; ++i) {
        int w = p[i].rotated ? r[i].h : r[i].w;
        int h = p[i].rotated ? r[i].w : r[i].h;
        if (!p[i].placed || p[i].x < 0 || p[i].y < 0 || p[i].x + w > width)
            return 1;
        area += r[i].w * r[i].h;
        for (int j = 0; j < i; ++j) {
            int wj = p[j].rotated ? r[j].h : r[j].w;
            int hj = p[j].rotated ? r[j].w : r[j].h;
            if (p[i].x < p[j].x + wj && p[j].x < p[i].x + w &&
                p[i].y < p[j].y + hj && p[j].y < p[i].y + h)
                return 2;
        }
    }
    return area == s->used_px ? 0 : 3;
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

int main(void) {
    static AtlasRect rects[NRECTS];
    static AtlasPos pos[NRECTS], ref[NRECTS];
    AtlasStats st, rst;

    /* Mixed workload: mostly tall, narrow sprites (the wasteful case for a
     * narrow strip) plus a quarter of wide, short blocks whose 90-degree
     * rotation can fill leftover shelf gaps. */
    lcg_state = 0x5eed1234u;
    for (int i = 0; i < NRECTS; ++i) {
        if (i % 4 == 3) {
            rects[i].w = 80 + lcg() % 41;    /* 80..120 wide  */
            rects[i].h = 12 + lcg() % 19;    /* 12..30  short */
        } else {
            rects[i].w = 4  + lcg() % 37;    /*  4..40  narrow */
            rects[i].h = 60 + lcg() % 101;   /* 60..160 tall   */
        }
    }

    const struct { const char *name; unsigned flags; } modes[] = {
        {"baseline (no sort, no rot)", 0u},
        {"height sort only",           ATLAS_SORT_HEIGHT},
        {"rotate only",                ATLAS_ALLOW_ROTATE},
        {"sort + rotate",              ATLAS_SORT_HEIGHT | ATLAS_ALLOW_ROTATE},
    };
    const int widths[2] = {128, 512};

    for (int wi = 0; wi < 2; ++wi) {
        int width = widths[wi];
        if (manual_pack(rects, NRECTS, width, ref, &rst) != 0) {
            printf("width %d: manual pack rejected a rect\n", width);
            continue;
        }
        double ref_occ = (double)rst.used_px / ((double)width * rst.height);
        printf("== strip width %d ==\n", width);
        printf("%-26s %10s %10s %8s %8s %10s %10s\n",
               "mode", "occupancy", "err vs hand", "shelves", "rotated",
               "time(us)", "verify");
        for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); ++m) {
            double t0 = now_us();
            int rc = 0;
            for (int k = 0; k < REPS; ++k)
                rc = atlas_pack_ex(rects, NRECTS, width, modes[m].flags, NULL,
                                   pos, &st, NULL);
            double elapsed = (now_us() - t0) / REPS;
            if (rc != 0) {
                printf("%-26s rejected (rc=%d)\n", modes[m].name, rc);
                continue;
            }
            int ok = validate(rects, NRECTS, width, pos, &st);
            int rotated = 0;
            for (int i = 0; i < NRECTS; ++i) rotated += pos[i].rotated;
            double occ = (double)st.used_px / ((double)width * st.height);
            int identical = 1;
            for (int i = 0; i < NRECTS; ++i) {
                if (pos[i].x != ref[i].x || pos[i].y != ref[i].y) {
                    identical = 0;
                    break;
                }
            }
            printf("%-26s %9.2f%% %9.2f%% %8d %8d %10.3f %10s\n",
                   modes[m].name, 100.0 * occ, 100.0 * (occ - ref_occ),
                   st.shelves, rotated, elapsed,
                   ok ? "INVALID" : (identical ? "ok=hand" : "ok"));
        }
        printf("hand-placed reference:     occ=%6.2f%% shelves=%d height=%d\n\n",
               100.0 * ref_occ, rst.shelves, rst.height);
    }
    return 0;
}
