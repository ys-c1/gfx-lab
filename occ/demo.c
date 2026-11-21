#define _POSIX_C_SOURCE 199309L
#include "occ.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { NSEG = 8, REPEAT = 20, REF_REPEAT = 3 };

/* Same relative geometry on every grid, so a hand calculation on the
 * unit square transfers directly: scale by S. */
static void make_segments(OccSeg *segs, int s) {
    const float f = (float)s;
    const float p[][4] = {
        {0.05f, 0.10f, 0.95f, 0.10f},  /* 0 horizontal, near top    */
        {0.10f, 0.05f, 0.10f, 0.95f},  /* 1 vertical, near left     */
        {0.20f, 0.80f, 0.80f, 0.20f},  /* 2 descending diagonal     */
        {0.20f, 0.20f, 0.80f, 0.80f},  /* 3 ascending diagonal      */
        {0.40f, 0.05f, 0.40f, 0.60f},  /* 4 vertical mid            */
        {0.05f, 0.50f, 0.60f, 0.50f},  /* 5 horizontal mid          */
        {0.60f, 0.60f, 0.95f, 0.95f},  /* 6 short diagonal          */
        {0.30f, 0.90f, 0.70f, 0.90f},  /* 7 horizontal near bottom  */
    };
    for (int i = 0; i < NSEG; ++i)
        for (int k = 0; k < 4; ++k)
            ((float*)&segs[i])[k] = p[i][k] * f;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int run_grid(int s) {
    const size_t cells = (size_t)s * (size_t)s;
    OccSeg *segs = (OccSeg *)malloc(sizeof(OccSeg) * NSEG);
    unsigned char *ref = (unsigned char *)malloc(cells);
    if (!segs || !ref) return 1;
    make_segments(segs, s);
    int bad = -1;

    /* ---- phase 1 timed (repeated; mean of successful runs) ---- */
    OccCov cov;
    double t0 = now_sec();
    int rc = OCC_OK;
    for (int r = 0; r < REPEAT; ++r) {
        if (r) occ_cov_free(&cov);
        rc = occ_compute(&cov, segs, NSEG, s, s, &bad);
        if (rc != OCC_OK) break;
    }
    double compute_ms = (now_sec() - t0) * 1000.0 / REPEAT;
    if (rc != OCC_OK) {
        printf("grid %d: compute failed seg %d: %s\n", s, bad, occ_strerror(rc));
        free(segs); free(ref);
        return 1;
    }

    /* ---- independent reference, per segment, timed ---- */
    double r0 = now_sec();
    float max_err = 0.0f;
    for (int rr = 0; rr < REF_REPEAT; ++rr) {
        max_err = 0.0f;
        for (int i = 0; i < NSEG; ++i) {
            int rb = -1;
            if (occ_reference_mask(ref, &segs[i], 1, s, s, &rb) != OCC_OK) {
                printf("grid %d: reference failed seg %d\n", s, rb);
                free(segs); free(ref); occ_cov_free(&cov);
                return 1;
            }
            long ref_cells = 0, diff = 0;
            const unsigned char *plane = cov.mask + (size_t)i * cells;
            for (size_t k = 0; k < cells; ++k) {
                ref_cells += ref[k];
                if (ref[k] != plane[k]) ++diff;
            }
            float ref_rate = (float)ref_cells / (float)cells;
            float err = fabsf(ref_rate - cov.rate[i]);
            if (err > max_err) max_err = err;
            if (rr == 0 && diff != 0)
                printf("grid %d seg %d: mask mismatch %zu cells\n", s, i, diff);
        }
    }
    double ref_ms = (now_sec() - r0) * 1000.0 / REF_REPEAT;

    /* ---- phase 2 timed ---- */
    OccAgg agg;
    double a0 = now_sec();
    for (int r = 0; r < REPEAT; ++r) {
        if (r) occ_agg_free(&agg);
        rc = occ_aggregate(&cov, &agg, &bad);
        if (rc != OCC_OK) break;
    }
    double agg_ms = (now_sec() - a0) * 1000.0 / REPEAT;
    if (rc != OCC_OK) {
        printf("grid %d: aggregate failed: %s (seg %d)\n", s, occ_strerror(rc), bad);
        free(segs); free(ref); occ_cov_free(&cov);
        return 1;
    }

    /* ---- aggregation off: compute-only path, output identical ---- */
    OccCov off;
    if (occ_compute(&off, segs, NSEG, s, s, &bad) != OCC_OK) return 1;
    int off_same = memcmp(cov.mask, off.mask, (size_t)NSEG * cells) == 0 &&
                   memcmp(cov.rate, off.rate, (size_t)NSEG * sizeof(float)) == 0;
    occ_cov_free(&off);

    /* ---- report ---- */
    printf("================ grid %d x %d, %d segments ================\n", s, s, NSEG);
    printf("phase1 compute: %.4f ms/run   reference: %.4f ms/run   phase2 aggregate: %.4f ms/run\n",
           compute_ms, ref_ms, agg_ms);
    printf("max coverage-rate error vs independent reference: %.3e\n", (double)max_err);
    printf("%-4s %10s %12s %10s %16s\n",
           "seg", "covered", "rate", "ref_rate", "added_in_union");
    for (int i = 0; i < NSEG; ++i) {
        long ref_cells = 0;
        if (occ_reference_mask(ref, &segs[i], 1, s, s, &bad) == OCC_OK)
            for (size_t k = 0; k < cells; ++k) ref_cells += ref[k];
        printf("%-4d %10d %12.6f %10.6f %16d\n",
               i, cov.covered[i], cov.rate[i],
               (double)ref_cells / (double)cells, agg.added[i]);
    }
    printf("aggregate: or_ops=%ld union_cells=%ld union_rate=%.6f aggregate_calls=%d\n",
           agg.or_ops, agg.union_cells,
           (double)agg.union_cells / (double)cells, REPEAT);
    printf("aggregation-off path keeps phase-1 output identical: %s\n",
           off_same ? "yes" : "NO");

    /* ---- traceable before/after difference list, per segment ---- */
    char path[64];
    snprintf(path, sizeof(path), "demo_%d_diff.txt", s);
    FILE *df = fopen(path, "w");
    if (df) {
        fprintf(df, "# grid %d x %d, per-segment merge trace\n", s, s);
        fprintf(df, "# before = segment plane alone; after = union up to and including this segment\n");
        fprintf(df, "# seg  covered_before  cells_newly_set  cumulative_union  new_cells(x,y)\n");
        long cumulative = 0;
        unsigned char *cum = (unsigned char *)calloc(cells, 1);
        for (int i = 0; i < NSEG; ++i) {
            const unsigned char *plane = cov.mask + (size_t)i * cells;
            int shown = 0;
            fprintf(df, "%-4d %15d %16d %16ld  ",
                    i, cov.covered[i], agg.added[i], cumulative + agg.added[i]);
            for (int y = 0; y < s; ++y)
                for (int x = 0; x < s; ++x) {
                    size_t k = (size_t)y * (size_t)s + (size_t)x;
                    if (plane[k] && !cum[k]) {
                        if (shown < 12) fprintf(df, "%s(%d,%d)", shown ? " " : "", x, y);
                        ++shown;
                    }
                }
            if (shown > 12) fprintf(df, " ... +%d more", shown - 12);
            fputc('\n', df);
            for (size_t k = 0; k < cells; ++k) cum[k] |= plane[k];
            cumulative += agg.added[i];
        }
        free(cum);
        fclose(df);
        printf("traceable per-segment diff list: %s\n", path);
    }

    snprintf(path, sizeof(path), "demo_%d_union.pgm", s);
    if (occ_write_pgm(agg.mask, s, s, path) == OCC_OK)
        printf("union mask preview: %s\n", path);
    snprintf(path, sizeof(path), "demo_%d_seg0.pgm", s);
    occ_write_pgm(cov.mask, s, s, path);

    occ_agg_free(&agg);
    occ_cov_free(&cov);
    free(ref);
    free(segs);
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= run_grid(256);
    rc |= run_grid(1024);
    return rc;
}
