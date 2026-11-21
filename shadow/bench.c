#define _POSIX_C_SOURCE 200809L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * Evidence harness for cast-distance truncation.
 *
 * Every case runs in its own forked child so the peak RSS reported belongs to
 * that case alone.  The child measures, the parent prints.
 *
 * For each map size (256, 1024) and each configuration it reports:
 *   - the distance tiers (edges in pixels and normalised) and brightness;
 *   - stage A: input segments, pieces, dropped / split / clipped, pieces per
 *     tier, kept vs original blocker length;
 *   - stage B: pixels, shadowed pixels and mean luminance per tier, plus the
 *     pixels beyond the cutoff;
 *   - accuracy against an independent per-pixel evaluator that does not call
 *     the library, and against the 8-bit PGM quantisation;
 *   - memory: map bytes, cast-piece bytes, accounted total, process peak RSS;
 *   - timing: truncation, re-cast, and the legacy full render;
 *   - a separate before/after table: legacy render vs truncated render.
 */

typedef struct {
    const char *name;
    int tiers;
    float cutoff;
    float split[SH_MAX_TIERS - 1];
    float bright[SH_MAX_TIERS];
} CfgSpec;

typedef struct {
    int size;
    int tiers;
    float cutoff;
    float split[SH_MAX_TIERS - 1];
    float bright[SH_MAX_TIERS];

    long input_segs, pieces, dropped, split_segs, clipped;
    long per_tier_pieces[SH_MAX_TIERS];
    double len_before, len_after;

    long pixels[SH_MAX_TIERS];
    long shadowed[SH_MAX_TIERS];
    double lum[SH_MAX_TIERS];
    long pixels_total, shadowed_total, beyond;
    double lum_total;

    double t_trunc_ms, t_render_ms, t_legacy_ms;

    double max_abs, mean_abs, rmse;
    long over_1e5, over_1_255;
    long pgm_diff_bytes;
    int pgm_max_diff;

    long before_shadowed;
    double before_lum;
    long changed_pixels;
    double ba_max, ba_rmse;

    long map_bytes, piece_bytes;
    long rss_base_kb, rss_peak_kb;

    int identical_when_off;
    int idempotent;
    double idem_max_shift;
    int ref_ok;
    unsigned long long map_hash;
} Res;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static long rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long v = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) { v = strtol(line + 6, NULL, 10); break; }
    }
    fclose(f);
    return v;
}

/* Scene in normalised coordinates: blockers at many distances from the light
 * so that some are entirely beyond the cutoff and long ones cross tiers. */
static const ShSeg SCENE[] = {
    {0.10f, 0.42f, 0.45f, 0.45f},
    {0.52f, 0.30f, 0.55f, 0.70f},
    {0.62f, 0.55f, 0.88f, 0.52f},
    {0.08f, 0.72f, 0.40f, 0.78f},
    {0.15f, 0.15f, 0.60f, 0.22f},
    {0.70f, 0.15f, 0.92f, 0.35f},
    {0.30f, 0.60f, 0.50f, 0.90f},
    {0.75f, 0.75f, 0.95f, 0.95f},
    {0.42f, 0.08f, 0.44f, 0.30f},
    {0.05f, 0.92f, 0.35f, 0.95f},
};
#define NSCENE ((int)(sizeof(SCENE) / sizeof(SCENE[0])))

static void scale_scene(ShSeg *out, int size) {
    for (int i = 0; i < NSCENE; ++i) {
        out[i].x0 = SCENE[i].x0 * (float)size; out[i].y0 = SCENE[i].y0 * (float)size;
        out[i].x1 = SCENE[i].x1 * (float)size; out[i].y1 = SCENE[i].y1 * (float)size;
    }
}

/* Independent evaluator: double-precision point-to-segment blocking test on
 * the *original* segments, with shadows switched off beyond the cutoff. */
static float reference_px(const ShSeg *segs, int n, const ShLight *l,
                          const CfgSpec *spec, int size, double px, double py) {
    double dx = px - (double)l->x, dy = py - (double)l->y;
    double d = sqrt(dx * dx + dy * dy);
    int shadowed = 0;
    if (d <= (double)spec->cutoff * (double)size) {
        for (int i = 0; i < n; ++i) {
            double sx = (double)segs[i].x1 - segs[i].x0;
            double sy = (double)segs[i].y1 - segs[i].y0;
            double den = dx * sy - dy * sx;
            if (fabs(den) < 1e-18) continue;
            double t = (((double)segs[i].x0 - (double)l->x) * sy -
                        ((double)segs[i].y0 - (double)l->y) * sx) / den;
            double u = (((double)segs[i].x0 - (double)l->x) * dy -
                        ((double)segs[i].y0 - (double)l->y) * dx) / den;
            if (t > 0.0 && t < 1.0 && u >= 0.0 && u <= 1.0) { shadowed = 1; break; }
        }
    }
    double lit = (double)l->intensity / (1.0 + d / (double)l->radius);
    if (!shadowed) return (float)lit;
    int tier = spec->tiers - 1;
    for (int k = 0; k < spec->tiers - 1; ++k)
        if (d <= (double)spec->split[k] * (double)size) { tier = k; break; }
    return (float)(lit * (double)spec->bright[tier]);
}

static int build_cfg(const CfgSpec *spec, int size, ShTruncCfg *cfg, ShError *err) {
    int on = 1;
    sh_cfg_default(cfg);
    if (!spec->tiers) return 0;               /* "off" */
    if (sh_cfg_set(cfg, SH_F_ENABLE, SH_TY_INT, &on, err) != SH_OK) return -1;
    if (sh_cfg_set(cfg, SH_F_TIER_COUNT, SH_TY_INT, &spec->tiers, err) != SH_OK) return -1;
    float cutoff = spec->cutoff * (float)size;
    if (sh_cfg_set(cfg, SH_F_CUTOFF, SH_TY_FLOAT, &cutoff, err) != SH_OK) return -1;
    for (int k = 0; k < spec->tiers - 1; ++k) {
        float v = spec->split[k] * (float)size;
        if (sh_cfg_set(cfg, (ShField)(SH_F_SPLIT_0 + k), SH_TY_FLOAT, &v, err) != SH_OK) return -1;
    }
    for (int k = 0; k < spec->tiers; ++k) {
        float v = spec->bright[k];
        if (sh_cfg_set(cfg, (ShField)(SH_F_BRIGHT_0 + k), SH_TY_FLOAT, &v, err) != SH_OK) return -1;
    }
    return 0;
}

static void fmt_tiers(char *buf, size_t n, const CfgSpec *spec, int size) {
    int off = 0;
    off += snprintf(buf + off, n - (size_t)off, "[0");
    for (int k = 0; k < spec->tiers - 1; ++k)
        off += snprintf(buf + off, n - (size_t)off, " %.0f", spec->split[k] * size);
    snprintf(buf + off, n - (size_t)off, " %.0f]", spec->cutoff * size);
}

static void fmt_bright(char *buf, size_t n, const CfgSpec *spec) {
    int off = 0;
    for (int k = 0; k < spec->tiers; ++k)
        off += snprintf(buf + off, n - (size_t)off, "%s%.2f", k ? "/" : "", spec->bright[k]);
}

static int run_case(int size, const CfgSpec *spec, Res *r) {
    memset(r, 0, sizeof(*r));
    r->size = size;
    r->tiers = spec->tiers;
    r->cutoff = spec->cutoff * (float)size;
    for (int k = 0; k < SH_MAX_TIERS - 1; ++k) r->split[k] = spec->split[k] * (float)size;
    for (int k = 0; k < SH_MAX_TIERS; ++k) r->bright[k] = spec->bright[k];

    ShSeg scene[NSCENE];
    scale_scene(scene, size);
    ShLight light = {0.50f * (float)size, 0.06f * (float)size,
                     0.28f * (float)size, 1.0f};
    int W = size, H = size;

    ShadowMap map, legacy;
    if (sh_init(&map, W, H) != 0 || sh_init(&legacy, W, H) != 0) return -1;
    ShTruncCfg cfg;
    ShError err;
    if (build_cfg(spec, size, &cfg, &err) != 0) return -1;

    long rss_base = rss_kb();

    /* ---- "off": must reproduce the legacy renderer bit for bit ---- */
    if (spec->tiers == 0) {
        for (int i = 0; i < NSCENE; ++i) {
            double dx = (double)scene[i].x1 - scene[i].x0, dy = (double)scene[i].y1 - scene[i].y0;
            r->len_before += sqrt(dx * dx + dy * dy);
        }
        ShSeg scene_copy[NSCENE];
        memcpy(scene_copy, scene, sizeof scene);
        r->input_segs = NSCENE;
        r->pieces = NSCENE;
        sh_render(&legacy, &light, scene, NSCENE);
        double t0 = now_ms();
        ShCastSet no_cs;
        if (sh_render_cast(&map, &light, scene, NSCENE, &cfg, &no_cs, NULL, &err) != SH_OK)
            return -1;
        r->t_legacy_ms = now_ms() - t0;
        r->t_render_ms = r->t_legacy_ms;
        r->identical_when_off = memcmp(map.buf, legacy.buf, (size_t)W * H * sizeof(float)) == 0;
        r->ref_ok = r->identical_when_off;
        for (long i = 0; i < (long)W * H; ++i) {
            double px = (double)(i % W) + 0.5, py = (double)(i / W) + 0.5;
            double dx = px - (double)light.x, dy = py - (double)light.y;
            double d = sqrt(dx * dx + dy * dy);
            double lit = (double)light.intensity / (1.0 + d / (double)light.radius);
            r->before_lum += legacy.buf[i];
            r->lum_total += map.buf[i];
            if ((double)legacy.buf[i] < lit - 1e-5) r->before_shadowed++;
            if ((double)map.buf[i] < lit - 1e-5) r->shadowed_total++;
        }
        r->before_lum /= (double)(W * H);
        r->lum_total /= (double)(W * H);
        r->pixels_total = (long)W * H;
        r->len_after = r->len_before;
        /* The evaluator is run on the raw scene: it must agree with the
         * legacy renderer exactly (this is the "don't regress" control). */
        for (long i = 0; i < (long)W * H; ++i) {
            double px = (double)(i % W) + 0.5, py = (double)(i / W) + 0.5;
            int sh = 0;
            for (int k = 0; k < NSCENE; ++k) {
                const ShSeg *g = &scene_copy[k];
                double rx = px - (double)light.x, ry = py - (double)light.y;
                double sx = (double)g->x1 - g->x0, sy = (double)g->y1 - g->y0;
                double den = rx * sy - ry * sx;
                if (fabs(den) < 1e-18) continue;
                double t = (((double)g->x0 - (double)light.x) * sy -
                            ((double)g->y0 - (double)light.y) * sx) / den;
                double u = (((double)g->x0 - (double)light.x) * ry -
                            ((double)g->y0 - (double)light.y) * rx) / den;
                if (t > 0.0 && t < 1.0 && u >= 0.0 && u <= 1.0) { sh = 1; break; }
            }
            double lit = (double)light.intensity /
                         (1.0 + sqrt((px - (double)light.x) * (px - (double)light.x) +
                                     (py - (double)light.y) * (py - (double)light.y)) /
                                (double)light.radius);
            double want = sh ? 0.0 : lit;
            double e = fabs((double)legacy.buf[i] - want);
            if (e > r->max_abs) r->max_abs = e;
            if (e > 1e-5) r->over_1e5++;
        }
        r->mean_abs = r->max_abs;
        r->rmse = r->max_abs;
        r->map_bytes = (long)W * H * (long)sizeof(float);
        r->piece_bytes = 0;
        r->rss_base_kb = rss_base;
        r->rss_peak_kb = rss_kb();
        for (int i = 0; i < W * H; ++i) {
            unsigned char a = (unsigned char)(fminf(1.0f, fmaxf(0.0f, map.buf[i])) * 255.0f + 0.5f);
            r->map_hash = (r->map_hash ^ a) * 1099511628211ULL;
        }
        sh_free(&map); sh_free(&legacy);
        return 0;
    }

    /* ---- stage A ---- */
    ShCastSet cs;
    double t0 = now_ms();
    if (sh_truncate(&cs, &light, scene, NSCENE, &cfg, &err) != SH_OK) return -1;
    r->t_trunc_ms = now_ms() - t0;

    /* idempotence: truncating the pieces again yields the same pieces */
    {
        ShSeg *again = (ShSeg*)malloc(sizeof(ShSeg) * (size_t)(cs.count ? cs.count : 1));
        for (int i = 0; i < cs.count; ++i) again[i] = cs.pieces[i].seg;
        ShCastSet cs2;
        if (sh_truncate(&cs2, &light, again, cs.count, &cfg, &err) != SH_OK) return -1;
        int same = cs2.count == cs.count;
        double worst = 0.0;
        for (int i = 0; same && i < cs.count; ++i) {
            double dx0 = (double)cs2.pieces[i].seg.x0 - cs.pieces[i].seg.x0;
            double dy0 = (double)cs2.pieces[i].seg.y0 - cs.pieces[i].seg.y0;
            double dx1 = (double)cs2.pieces[i].seg.x1 - cs.pieces[i].seg.x1;
            double dy1 = (double)cs2.pieces[i].seg.y1 - cs.pieces[i].seg.y1;
            double e = sqrt(dx0 * dx0 + dy0 * dy0);
            double e1 = sqrt(dx1 * dx1 + dy1 * dy1);
            if (e1 > e) e = e1;
            if (e > worst) worst = e;
            if (e > 1e-3) same = 0;
        }
        r->idem_max_shift = worst;
        r->idempotent = same;
        free(again);
        sh_castset_free(&cs2);
    }

    r->input_segs = cs.input_count;
    r->pieces = cs.count;
    r->dropped = cs.dropped;
    r->split_segs = cs.split;
    r->clipped = cs.clipped;
    for (int k = 0; k < cs.tier_count; ++k) r->per_tier_pieces[k] = cs.per_tier[k];
    r->len_before = cs.len_before;
    r->len_after = cs.len_after;

    /* ---- stage B, timed (best of 3) ---- */
    ShRenderStats st;
    r->t_render_ms = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
        double t1 = now_ms();
        if (sh_render_truncated(&map, &light, &cs, &cfg, &st, &err) != SH_OK) return -1;
        double dt = now_ms() - t1;
        if (dt < r->t_render_ms) r->t_render_ms = dt;
    }
    /* Map + cast set + legacy map are all resident here; rss_peak is taken at
     * the end of the case (VmHWM only grows), so it covers this plus the
     * comparison work below. */
    r->map_bytes = (long)W * H * (long)sizeof(float);
    r->piece_bytes = (long)cs.count * (long)sizeof(ShCastPiece);
    r->rss_base_kb = rss_base;
    r->pixels_total = st.pixels_total;
    r->shadowed_total = st.shadowed_total;
    r->beyond = st.beyond_cutoff;
    r->lum_total = st.lum_sum_total / (double)(st.pixels_total ? st.pixels_total : 1);
    for (int k = 0; k < SH_MAX_TIERS; ++k) {
        r->pixels[k] = st.pixels[k];
        r->shadowed[k] = st.shadowed[k];
        r->lum[k] = st.pixels[k] ? st.lum_sum[k] / (double)st.pixels[k] : 0.0;
    }

    /* ---- legacy full render for the before/after table (best of 3) ---- */
    r->t_legacy_ms = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
        double t2 = now_ms();
        if (sh_render(&legacy, &light, scene, NSCENE) != 0) return -1;
        double dt = now_ms() - t2;
        if (dt < r->t_legacy_ms) r->t_legacy_ms = dt;
    }

    /* ---- accuracy against the independent evaluator ---- */
    {
        double se = 0.0, sa = 0.0;
        int max_byte = 0;
        long pgm_diff = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float want = reference_px(scene, NSCENE, &light, spec, size,
                                          (double)x + 0.5, (double)y + 0.5);
                float got = map.buf[(size_t)y * W + x];
                double e = fabs((double)got - (double)want);
                if (e > r->max_abs) r->max_abs = e;
                se += e * e;
                sa += e;
                if (e > 1e-5) r->over_1e5++;
                if (e > 1.0 / 255.0) r->over_1_255++;
                int gb = (int)(fminf(1.0f, fmaxf(0.0f, got)) * 255.0f + 0.5f);
                int wb = (int)(fminf(1.0f, fmaxf(0.0f, want)) * 255.0f + 0.5f);
                int db = abs(gb - wb);
                if (db) pgm_diff++;
                if (db > max_byte) max_byte = db;
            }
        }
        r->rmse = sqrt(se / (double)((long)W * H));
        r->mean_abs = sa / (double)((long)W * H);
        r->pgm_diff_bytes = pgm_diff;
        r->pgm_max_diff = max_byte;
        r->ref_ok = (r->over_1e5 == 0);
    }

    /* ---- before/after truncation ---- */
    {
        double se = 0.0, sa_before = 0.0;
        for (long i = 0; i < (long)W * H; ++i) {
            float b = legacy.buf[i], a = map.buf[i];
            double e = fabs((double)a - (double)b);
            if (e > 1e-9) r->changed_pixels++;
            if (e > r->ba_max) r->ba_max = e;
            se += e * e;
            sa_before += b;
        }
        r->ba_rmse = sqrt(se / (double)((long)W * H));
        r->before_lum = sa_before / (double)((long)W * H);
        for (long i = 0; i < (long)W * H; ++i) {
            double px = (double)(i % W) + 0.5, py = (double)(i / W) + 0.5;
            double dx = px - (double)light.x, dy = py - (double)light.y;
            double lit = (double)light.intensity /
                         (1.0 + sqrt(dx * dx + dy * dy) / (double)light.radius);
            if ((double)legacy.buf[i] < lit - 1e-5) r->before_shadowed++;
        }
    }

    /* 8-bit hash of the truncated map (for cross-run comparisons) */
    for (long i = 0; i < (long)W * H; ++i) {
        unsigned char b = (unsigned char)(fminf(1.0f, fmaxf(0.0f, map.buf[i])) * 255.0f + 0.5f);
        r->map_hash = (r->map_hash ^ b) * 1099511628211ULL;
    }

    /* VmHWM covers map + cast set + legacy map + evaluator temporaries */
    r->rss_peak_kb = rss_kb();
    sh_castset_free(&cs);
    sh_free(&map);
    sh_free(&legacy);
    return 0;
}

static int fork_case(int size, const CfgSpec *spec, Res *out) {
    int fd[2];
    if (pipe(fd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(fd[0]);
        Res r;
        int rc = run_case(size, spec, &r);
        ssize_t n = write(fd[1], &r, sizeof r);
        (void)n;
        close(fd[1]);
        _exit(rc == 0 ? 0 : 1);
    }
    close(fd[1]);
    size_t got = 0;
    while (got < sizeof *out) {
        ssize_t n = read(fd[0], (char*)out + got, sizeof *out - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return (got == sizeof *out && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static void print_tiers_and_pieces(const CfgSpec *specs, int nspec, const Res *res, int off) {
    printf("  %-26s %-14s %-13s %6s %6s %6s %6s   %s\n",
           "config", "cutoff(px)", "tiers(px)", "in", "pieces", "drop", "split", "pieces per tier");
    for (int i = 0; i < nspec; ++i) {
        const Res *r = &res[i];
        if (specs[i].tiers == 0) {
            printf("  %-26s %-14s %-13s %6ld %6s %6s %6s   %s\n",
                   specs[i].name, "-", "-", r->input_segs, "-", "-", "-", "-");
            continue;
        }
        char tiers[48], bright[64], per[64];
        fmt_tiers(tiers, sizeof tiers, &specs[i], r->size);
        fmt_bright(bright, sizeof bright, &specs[i]);
        int o = 0;
        for (int k = 0; k < r->tiers; ++k)
            o += snprintf(per + o, sizeof per - (size_t)o, "%s%ld", k ? "/" : "", r->per_tier_pieces[k]);
        printf("  %-26s %-14.0f %-13s %6ld %6ld %6ld %6ld   %s\n",
               specs[i].name, r->cutoff, tiers, r->input_segs, r->pieces,
               r->dropped, r->split_segs, per);
        (void)bright;
    }
    (void)off;
}

int main(void) {
    CfgSpec specs[] = {
        {"off (legacy)",                       0, 0.00f, {0, 0, 0},           {0, 0, 0, 0}},
        {"1 tier, cutoff .60",                 1, 0.60f, {0, 0, 0},           {0, 0, 0, 0}},
        {"2 tiers, cutoff .60",                2, 0.60f, {0.30f, 0, 0},       {0, 0.5f, 0, 0}},
        {"3 tiers, cutoff .60",                3, 0.60f, {0.22f, 0.42f, 0},   {0, 0.35f, 0.75f, 0}},
        {"3 tiers, short cutoff .40",          3, 0.40f, {0.15f, 0.28f, 0},   {0, 0.35f, 0.75f, 0}},
        {"4 tiers, cutoff .60",                4, 0.60f, {0.15f, 0.28f, 0.44f}, {0, 0.25f, 0.55f, 0.85f}},
    };
    const int nspec = (int)(sizeof(specs) / sizeof(specs[0]));
    const int sizes[] = {256, 1024};

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        int size = sizes[s];
        Res res[16];
        int bad = 0;
        for (int i = 0; i < nspec; ++i)
            if (fork_case(size, &specs[i], &res[i]) != 0) { bad = 1; break; }
        if (bad) { fprintf(stderr, "case failed at size %d\n", size); return 1; }

        printf("\n================================================================================\n");
        printf("Map %dx%d   light=(%.1f,%.1f) radius=%.1f  %d blockers\n",
               size, size, 0.50 * size, 0.06 * size, 0.28 * size, NSCENE);
        printf("================================================================================\n");

        printf("\n[1] Configuration, piece counts (stage A)\n");
        print_tiers_and_pieces(specs, nspec, res, 0);
        for (int i = 0; i < nspec; ++i) {
            if (specs[i].tiers == 0) continue;
            char bright[64];
            fmt_bright(bright, sizeof bright, &specs[i]);
            printf("      %-26s brightness per tier %-4s  clipped %ld  length %.1f -> %.1f px\n",
                   specs[i].name, bright, res[i].clipped, res[i].len_before, res[i].len_after);
        }

        printf("\n[2] Luminance and shadowing (stage B)\n");
        printf("  %-26s %8s %8s %8s %10s  %s\n",
               "config", "lum", "shadowed", "beyond", "lum before", "pixels/shadowed/mean lum per tier");
        for (int i = 0; i < nspec; ++i) {
            const Res *r = &res[i];
            char per[128];
            int o = 0;
            for (int k = 0; k < (r->tiers ? r->tiers : 1); ++k)
                o += snprintf(per + o, sizeof per - (size_t)o, "%s%ld/%ld/%.3f",
                              k ? "  " : "", r->pixels[k], r->shadowed[k], r->lum[k]);
            printf("  %-26s %8.4f %8ld %8ld %10.4f  %s\n",
                   specs[i].name, r->lum_total, r->shadowed_total, r->beyond,
                   r->before_lum, per);
        }

        printf("\n[3] Accuracy vs independent per-pixel evaluator\n");
        printf("  %-26s %8s %10s %9s %9s %9s %8s %6s\n",
               "config", "max abs", "mean abs", "rmse", ">1e-5", ">1/255", "pgm diff", "max 8b");
        for (int i = 0; i < nspec; ++i) {
            const Res *r = &res[i];
            if (specs[i].tiers == 0) {
                printf("  %-26s %8.3g %10.3g %9.3g %9s %9s %8s %6s   %s\n",
                       specs[i].name, r->max_abs, r->mean_abs, r->rmse, "-", "-", "-", "-",
                       r->identical_when_off ? "identical to legacy" : "DIFFERS");
                continue;
            }
            printf("  %-26s %8.3g %10.3g %9.3g %9ld %9ld %8ld %6d   %s\n",
                   specs[i].name, r->max_abs, r->mean_abs, r->rmse,
                   r->over_1e5, r->over_1_255, r->pgm_diff_bytes, r->pgm_max_diff,
                   r->idempotent ? "idempotent" : "NOT idempotent");
        }

        printf("\n[4] Memory\n");
        printf("  %-26s %10s %10s %10s %10s %10s %10s\n",
               "config", "map B", "pieces B", "accounted", "rss base", "rss peak", "rss delta");
        for (int i = 0; i < nspec; ++i) {
            const Res *r = &res[i];
            long acc = r->map_bytes + r->piece_bytes;
            printf("  %-26s %10ld %10ld %10ld %9ldK %9ldK %9ldK\n",
                   specs[i].name, r->map_bytes, r->piece_bytes, acc,
                   r->rss_base_kb, r->rss_peak_kb, r->rss_peak_kb - r->rss_base_kb);
        }
        printf("  (sizeof(ShCastPiece)=%zu B; pieces column is count * that size)\n",
               sizeof(ShCastPiece));

        printf("\n[5] Timing (best of 3)\n");
        printf("  %-26s %10s %10s %10s %8s\n",
               "config", "truncate", "re-cast", "legacy", "ms/MPx");
        for (int i = 0; i < nspec; ++i) {
            const Res *r = &res[i];
            double total = specs[i].tiers ? r->t_trunc_ms + r->t_render_ms : r->t_render_ms;
            printf("  %-26s %8.3fms %8.3fms %8.3fms %8.3f\n",
                   specs[i].name, specs[i].tiers ? r->t_trunc_ms : 0.0,
                   r->t_render_ms, r->t_legacy_ms,
                   total * (1e6 / (double)(size * size)));
        }

        printf("\n[6] Before vs after truncation (legacy render -> truncated render)\n");
        printf("  %-26s %10s %10s %10s %8s %8s %10s\n",
               "config", "shadowed-", "shadowed+", "lum-", "lum+", "changed", "max diff");
        for (int i = 0; i < nspec; ++i) {
            const Res *r = &res[i];
            if (specs[i].tiers == 0) {
                printf("  %-26s %10s %10s %8.4f %8.4f %10s %10s\n",
                       specs[i].name, "-", "-", r->before_lum, r->lum_total, "0", "0");
                continue;
            }
            printf("  %-26s %10ld %10ld %8.4f %8.4f %10ld %10.4f\n",
                   specs[i].name, r->before_shadowed, r->shadowed_total,
                   r->before_lum, r->lum_total, r->changed_pixels, r->ba_max);
        }
        printf("  (shadowed- = legacy, shadowed+ = truncated; changed = pixels differing, max diff in [0,1])\n");
    }

    printf("\nNotes\n");
    printf("  Every case ran in its own process, so rss peak is that case's VmHWM.\n");
    printf("  Accuracy compares the truncated pipeline with a double-precision evaluator\n");
    printf("  over the original segments and the same cutoff/tier rules; 0 >1e-5 means\n");
    printf("  the two-stage pipeline is exact under its documented semantics.\n");
    printf("  Before/after is the intentional change: shadows stop at the cutoff and the\n");
    printf("  brightness per receiver tier lifts shadowed pixels from black.\n");
    return 0;
}
