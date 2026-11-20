#include "atlas.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NTILES 24

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static int scaled(int v, double r) {
    int s = (int)floor((double)v * r + 0.5);
    return s < 1 ? 1 : s;
}

/* Uniform proportional scaling: every tile gets the same ratio.  This is the
 * hand-made reference the tiered layout is measured against.  We use the
 * ratio that matches the tiered layout's occupied area, so both layouts
 * carry the same amount of content. */
static int manual_pack(const AtlasRect *src, int n, int width, double ratio,
                       AtlasPos *out, AtlasStats *stats) {
    AtlasRect *r = (AtlasRect *)malloc(sizeof(AtlasRect) * (size_t)n);
    if (!r) return -1;
    for (int i = 0; i < n; ++i) {
        r[i].w = scaled(src[i].w, ratio);
        r[i].h = scaled(src[i].h, ratio);
    }
    int rc = atlas_pack(r, n, width, out, stats);
    free(r);
    return rc;
}

static double manual_ratio_for(const AtlasRect *r, int n, long long target_area) {
    long long orig = 0;
    for (int i = 0; i < n; ++i) orig += (long long)r[i].w * r[i].h;
    if (orig <= 0 || target_area <= 0) return 1.0;
    return sqrt((double)target_area / (double)orig);
}

static int count_placements(const AtlasPos *p, int n) {
    int c = 0;
    for (int i = 0; i < n; ++i) c += p[i].placed ? 1 : 0;
    return c;
}

static void run_width(FILE *out, const AtlasRect *tiles, int n, int width,
                      const AtlasScaleOpt *opt) {
    AtlasPos *tiered = (AtlasPos *)calloc((size_t)n, sizeof(AtlasPos));
    AtlasPos *manual = (AtlasPos *)calloc((size_t)n, sizeof(AtlasPos));
    AtlasStats ts, ms, un;
    AtlasLayout layout = {0};
    AtlasError err;

    /* Baseline: the untouched shelf layout, with no scaling at all. */
    if (atlas_pack_tiered(tiles, n, width, NULL, tiered, &un, NULL, &err) != 0) {
        fprintf(out, "width %d: baseline packing failed: %s\n", width, err.message);
        free(tiered); free(manual);
        return;
    }

    /* Warm up once, then time a fixed number of repetitions. */
    if (atlas_pack_tiered(tiles, n, width, opt, tiered, &ts, &layout, &err) != 0) {
        fprintf(out, "width %d: tiered packing failed: %s (tier %d)\n",
                width, err.message, err.index);
        atlas_layout_free(&layout);
        free(tiered); free(manual);
        return;
    }
    long long tiered_area = ts.used_px;
    atlas_layout_free(&layout);

    enum { REPS = 50000 };
    double t0 = now_ms();
    for (int k = 0; k < REPS; ++k) {
        atlas_layout_free(&layout);
        if (atlas_pack_tiered(tiles, n, width, opt, tiered, &ts, &layout, &err) != 0) {
            fprintf(out, "width %d: tiered packing failed: %s\n", width, err.message);
            atlas_layout_free(&layout);
            free(tiered); free(manual);
            return;
        }
    }
    double tiered_ms = (now_ms() - t0) / REPS;

    /* The manual layout scales every tile by the same ratio, picked so that
     * both layouts carry the same total content area. */
    double ratio = manual_ratio_for(tiles, n, tiered_area);
    int rc = manual_pack(tiles, n, width, ratio, manual, &ms);
    t0 = now_ms();
    for (int k = 0; k < REPS; ++k)
        rc |= manual_pack(tiles, n, width, ratio, manual, &ms);
    double manual_ms = (now_ms() - t0) / REPS;

    fprintf(out, "width=%d tiles=%d\n", width, n);
    fprintf(out, "  unscaled: shelf packing at original sizes\n");
    fprintf(out, "    used=%d shelves=%d height=%d bin=%lld "
                 "waste=%.2f%% occupancy=%.2f%%\n",
            un.used_px, un.shelves, un.height, un.bin_px,
            un.waste * 100.0, un.occupancy * 100.0);
    fprintf(out, "  tiered : area tiers, largest scaled hardest\n");
    fprintf(out, "    tiers=%d used=%d shelves=%d height=%d bin=%lld "
                 "waste=%.2f%% occupancy=%.2f%% time=%.3f us\n",
            ts.scale_tiers, ts.used_px, ts.shelves, ts.height, ts.bin_px,
            ts.waste * 100.0, ts.occupancy * 100.0, tiered_ms * 1000.0);
    if (rc == ATLAS_OK) {
        fprintf(out, "  manual : one uniform ratio for every tile\n");
        fprintf(out, "    ratio=%.4f used=%d shelves=%d height=%d bin=%lld "
                     "waste=%.2f%% occupancy=%.2f%% time=%.3f us\n",
                ratio, ms.used_px, ms.shelves, ms.height, ms.bin_px,
                ms.waste * 100.0, ms.occupancy * 100.0, manual_ms * 1000.0);
        fprintf(out, "  tiered vs manual: waste %+.2f pp, occupancy %+.2f pp, "
                     "time %+.1f%%\n",
                (ts.waste - ms.waste) * 100.0,
                (ts.occupancy - ms.occupancy) * 100.0,
                manual_ms > 0.0 ? (tiered_ms - manual_ms) / manual_ms * 100.0 : 0.0);
    } else {
        fprintf(out, "  manual : did not fit (scaled tiles exceed width %d)\n", width);
    }
    fprintf(out, "  placements: tiered=%d manual=%d of %d\n\n",
            count_placements(tiered, n), count_placements(manual, n), n);

    atlas_layout_free(&layout);
    free(tiered);
    free(manual);
}

int main(void) {
    /* Deliberately mixed sizes: a few very large tiles plus many small ones,
     * which is the case where plain shelf packing wastes most of its area. */
    AtlasRect tiles[NTILES];
    /* Largest tile is 112 px wide so it still fits a 128 px atlas after the
     * hardest tier ratio (0.60 -> 67 px). */
    static const int base[8][2] = {
        {112,112},{96,80},{80,80},{72,48},{56,56},{40,32},{24,24},{12,8}
    };
    for (int i = 0; i < NTILES; ++i) {
        tiles[i].w = base[i % 8][0];
        tiles[i].h = base[i % 8][1];
    }

    AtlasScaleOpt opt;
    atlas_scale_opt_init(&opt, 4);

    FILE *out = fopen("demo.txt", "wb");
    if (!out) { perror("demo.txt"); return 1; }

    fprintf(out, "atlas demo: area-tiered scaling vs manual proportional scaling\n");
    fprintf(out, "tier ratios (largest area first):");
    for (int t = 0; t < opt.tier_count; ++t) fprintf(out, " %.2f", opt.ratio[t]);
    fprintf(out, "\n\n");

    run_width(out, tiles, NTILES, 128, &opt);
    run_width(out, tiles, NTILES, 512, &opt);

    fclose(out);

    /* Also show the plain layout and the tier assignment on stdout. */
    AtlasPos p[NTILES];
    AtlasStats s;
    AtlasLayout layout = {0};
    AtlasError err;
    if (atlas_pack_tiered(tiles, NTILES, 128, &opt, p, &s, &layout, &err) == 0) {
        printf("tiers=%d scaled_tiles=%d used=%d\n",
               s.scale_tiers, s.scaled_tiles, s.used_px);
        for (int i = 0; i < NTILES && i < 4; ++i)
            printf("  tile %d: %dx%d tier=%d ratio=%.2f -> %dx%d at %d,%d\n",
                   i, layout.tiles[i].src.w, layout.tiles[i].src.h,
                   layout.tiles[i].tier, layout.tiles[i].ratio,
                   layout.tiles[i].scaled.w, layout.tiles[i].scaled.h,
                   layout.tiles[i].pos.x, layout.tiles[i].pos.y);
        atlas_layout_free(&layout);
    }
    printf("wrote demo.txt\n");
    return 0;
}
