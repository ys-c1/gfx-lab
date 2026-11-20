#include "atlas.h"
#include <stdio.h>
#include <time.h>

enum { N = 2000, REPS = 5 };

/* Best-of-REPS CPU time of one pack call, in milliseconds. */
static double time_ms(int maxrect, const AtlasRect *r, int width, AtlasPos *p,
                      AtlasStats *s, int *rc_out) {
    double best = 0.0;
    int rc = 0;
    for (int i = 0; i < REPS; ++i) {
        clock_t t0 = clock();
        rc = maxrect ? atlas_pack_maxrect(r, N, width, p, s, NULL)
                     : atlas_pack(r, N, width, p, s);
        clock_t dt = clock() - t0;
        if (rc != 0) break;
        double ms = (double)dt * 1000.0 / (double)CLOCKS_PER_SEC;
        if (i == 0 || ms < best) best = ms;
    }
    *rc_out = rc;
    return best;
}

typedef struct {
    double waste;   /* fraction */
    int strips;
    int height;
    int rotated;
    double ms;
} Row;

static int measure(int width, const AtlasRect *r, int maxrect, Row *out) {
    AtlasPos p[N];
    AtlasStats s;
    int rc = 0;
    double ms = time_ms(maxrect, r, width, p, &s, &rc);
    if (rc != 0) return rc;
    out->waste = s.waste;
    out->strips = s.shelves;
    out->height = s.height;
    out->rotated = s.rotated;
    out->ms = ms;
    return 0;
}

int main(void) {
    AtlasRect r[N];
    atlas_make_rects(r, N, 8, 64, 20261121u);

    Row greedy128, greedy512, mr128, mr512;
    if (measure(128, r, 0, &greedy128) || measure(512, r, 0, &greedy512) ||
        measure(128, r, 1, &mr128) || measure(512, r, 1, &mr512)) {
        printf("packing failed\n");
        return 1;
    }
    double lb128 = atlas_waste_lower_bound(r, N, 128);
    double ub128 = atlas_waste_upper_bound(r, N, 128);
    double lb512 = atlas_waste_lower_bound(r, N, 512);
    double ub512 = atlas_waste_upper_bound(r, N, 512);

    printf("atlas: %d rects, both sides 8..64, identical input for every row\n", N);
    printf("strip count = shelf rows (greedy) / distinct item rows (maxrect)\n\n");
    printf("packer   width   waste     strips   height   rotated   time\n");
    printf("-------------------------------------------------------------\n");
    printf("greedy   128   %6.2f%%   %6d   %6d         -   %6.2f ms\n",
           100.0 * greedy128.waste, greedy128.strips, greedy128.height, greedy128.ms);
    printf("greedy   512   %6.2f%%   %6d   %6d         -   %6.2f ms\n",
           100.0 * greedy512.waste, greedy512.strips, greedy512.height, greedy512.ms);
    printf("maxrect  128   %6.2f%%   %6d   %6d   %7d   %6.2f ms\n",
           100.0 * mr128.waste, mr128.strips, mr128.height, mr128.rotated, mr128.ms);
    printf("maxrect  512   %6.2f%%   %6d   %6d   %7d   %6.2f ms\n",
           100.0 * mr512.waste, mr512.strips, mr512.height, mr512.rotated, mr512.ms);
    printf("\nwaste bounds (area/largest-min-side is the floor, NFDH the ceiling)\n");
    printf("  width 128: [%6.2f%%, %6.2f%%]   width 512: [%6.2f%%, %6.2f%%]\n",
           100.0 * lb128, 100.0 * ub128, 100.0 * lb512, 100.0 * ub512);
    printf("\n128 -> 512: greedy waste %+.2f pts, maxrect waste %+.2f pts,\n",
           100.0 * (greedy512.waste - greedy128.waste),
           100.0 * (mr512.waste - mr128.waste));
    printf("            greedy strips %d -> %d, maxrect time %.2f -> %.2f ms\n",
           greedy128.strips, greedy512.strips, mr128.ms, mr512.ms);
    printf("maxrect vs greedy: %+.2f pts at 128, %+.2f pts at 512\n",
           100.0 * (greedy128.waste - mr128.waste),
           100.0 * (greedy512.waste - mr512.waste));
    return 0;
}
