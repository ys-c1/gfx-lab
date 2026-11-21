#define _POSIX_C_SOURCE 199309L
#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* error strings                                                      */
/* ------------------------------------------------------------------ */
const char *sh_strerror(int code) {
    switch (code) {
    case SH_OK:         return "ok";
    case SH_EINVAL:     return "invalid argument";
    case SH_ERANGE:     return "parameter out of range";
    case SH_ECONFLICT:  return "conflicting color configuration";
    case SH_ENOSWATCH:  return "unknown color swatch";
    default:            return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* RGB frame                                                          */
/* ------------------------------------------------------------------ */
int sh_frame_init(ShFrame *f, int w, int h) {
    if (!f || w <= 0 || h <= 0) return SH_EINVAL;
    f->rgb = (unsigned char *)calloc((size_t)w * (size_t)h, 3);
    if (!f->rgb) return SH_EINVAL;
    f->w = w; f->h = h;
    return SH_OK;
}

void sh_frame_free(ShFrame *f) {
    if (!f) return;
    free(f->rgb); f->rgb = NULL; f->w = 0; f->h = 0;
}

int sh_frame_fill(ShFrame *f, unsigned char r, unsigned char g, unsigned char b) {
    if (!f || !f->rgb) return SH_EINVAL;
    size_t n = (size_t)f->w * (size_t)f->h;
    for (size_t i = 0; i < n; ++i) {
        f->rgb[i * 3 + 0] = r;
        f->rgb[i * 3 + 1] = g;
        f->rgb[i * 3 + 2] = b;
    }
    return SH_OK;
}

int sh_write_ppm_f(const ShFrame *f, const char *path) {
    if (!f || !f->rgb || !path) return SH_EINVAL;
    FILE *fp = fopen(path, "wb");
    if (!fp) return SH_EINVAL;
    fprintf(fp, "P6\n%d %d\n255\n", f->w, f->h);
    if (fwrite(f->rgb, 1, (size_t)f->w * (size_t)f->h * 3, fp) !=
        (size_t)f->w * (size_t)f->h * 3) { fclose(fp); return SH_EINVAL; }
    fclose(fp);
    return SH_OK;
}

/* ------------------------------------------------------------------ */
/* color cards                                                        */
/* ------------------------------------------------------------------ */
typedef struct { const char *name; ShColor c; } Swatch;
static const Swatch SWATCHES[] = {
    {"warm-white", {255, 236, 200}},
    {"cool-white", {201, 226, 255}},
    {"candle",     {255, 160,  60}},
    {"golden",     {255, 201,  92}},
    {"dusk",       {190, 120, 170}},
    {"emerald",    { 80, 220, 150}},
    {"sky",        {110, 170, 255}},
    {"rose",       {255, 120, 140}},
};

int sh_swatch_count(void) { return (int)(sizeof(SWATCHES) / sizeof(SWATCHES[0])); }

int sh_swatch_get(int id, ShColor *out, char *name, int name_cap) {
    if (!out || id < 0 || id >= sh_swatch_count()) return SH_ENOSWATCH;
    *out = SWATCHES[id].c;
    if (name && name_cap > 0) {
        snprintf(name, (size_t)name_cap, "%s", SWATCHES[id].name);
        name[name_cap - 1] = '\0';
    }
    return SH_OK;
}

static const ShColor PRESETS[SH_PRESET_COUNT] = {
    {  0,   0,   0}, /* SH_PRESET_NONE (sentinel, never selected) */
    {255, 224, 178}, /* warm   */
    {178, 210, 255}, /* cool   */
    {255, 196,  90}, /* golden */
    {176, 110, 160}, /* dusk   */
};

/* ------------------------------------------------------------------ */
/* tune validation / color resolution                                 */
/* ------------------------------------------------------------------ */
int sh_tune_check(const ShTune *t, char *side_a, int cap_a, char *side_b, int cap_b) {
    if (!t) return SH_EINVAL;
    if (!t->enable) return SH_OK; /* switch off: nothing can conflict */

    /* Exactly one color source is allowed. "Configured" detection is safe
     * against a zero-initialized ShTune: preset has an explicit NONE=0
     * sentinel, RGB counts only when non-black, and the swatch card counts
     * only when the selector actually points at it (a bare swatch_id from
     * zero-init without the SWATCH selector is ignored). Two configured
     * sources => SH_ECONFLICT naming both sides. */
    static const char *SRC_NAME[] = {"color.rgb", "preset", "swatch"};
    int set[3];
    set[SH_SRC_RGB]    = (t->color.r || t->color.g || t->color.b) ? 1 : 0;
    set[SH_SRC_PRESET] = (t->preset != SH_PRESET_NONE) ? 1 : 0;
    /* swatch is "configured" when the selector actually picks it; a bare
     * swatch_id left at 0 by zero-init without the SWATCH selector is
     * intentionally ignored so {0}-initialized tunes stay conflict-free. */
    set[SH_SRC_SWATCH] = (t->color_source == SH_SRC_SWATCH) ? 1 : 0;

    int n_set = set[0] + set[1] + set[2];
    if (n_set > 1) {
        int first = -1, second = -1;
        for (int i = 0; i < 3; ++i) if (set[i]) {
            if (first < 0) first = i; else { second = i; break; }
        }
        if (side_a && cap_a > 0) snprintf(side_a, (size_t)cap_a, "%s", SRC_NAME[first]);
        if (side_b && cap_b > 0) snprintf(side_b, (size_t)cap_b, "%s", SRC_NAME[second]);
        return SH_ECONFLICT;
    }
    if (n_set == 0) {
        if (side_a && cap_a > 0) snprintf(side_a, (size_t)cap_a, "%s", "color.rgb/preset/swatch");
        if (side_b && cap_b > 0) snprintf(side_b, (size_t)cap_b, "%s", "<unset>");
        return SH_EINVAL;
    }
    if (t->color_source < 0 || t->color_source > 2) return SH_EINVAL;
    if (!set[t->color_source]) {
        /* selector points at a source whose carrying field was not set */
        if (side_a && cap_a > 0) snprintf(side_a, (size_t)cap_a, "%s", "color_source");
        if (side_b && cap_b > 0) snprintf(side_b, (size_t)cap_b, "%s", SRC_NAME[t->color_source]);
        return SH_ECONFLICT;
    }
    if (t->shadow_strength < 0.0f || t->shadow_strength > 1.0f) return SH_ERANGE;
    if (t->penumbra < 0.0f || t->penumbra > 64.0f) return SH_ERANGE;
    if (t->color_source == SH_SRC_PRESET &&
        (t->preset <= SH_PRESET_NONE || t->preset >= SH_PRESET_COUNT)) return SH_ERANGE;
    if (t->color_source == SH_SRC_SWATCH &&
        (t->swatch_id < 0 || t->swatch_id >= sh_swatch_count())) return SH_ENOSWATCH;
    return SH_OK;
}

int sh_resolve_color(const ShTune *t, ShColor *out) {
    if (!t || !out) return SH_EINVAL;
    char a[32], b[32];
    int rc = sh_tune_check(t, a, sizeof a, b, sizeof b);
    if (rc < 0) return rc;
    if (!t->enable) { *out = (ShColor){0, 0, 0}; return SH_OK; }
    switch (t->color_source) {
    case SH_SRC_RGB:    *out = t->color; return SH_OK;
    case SH_SRC_PRESET: *out = PRESETS[t->preset]; return SH_OK;
    case SH_SRC_SWATCH: return sh_swatch_get(t->swatch_id, out, NULL, 0);
    default:            return SH_EINVAL;
    }
}

/* ------------------------------------------------------------------ */
/* geometry: exact ray/segment intersection (same math as shadow.c)   */
/* ------------------------------------------------------------------ */
static int blocked(const ShSeg *s, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}

/* distance from point to segment */
static float point_seg_dist(float px, float py, const ShSeg *s) {
    float vx = s->x1 - s->x0, vy = s->y1 - s->y0;
    float wx = px - s->x0, wy = py - s->y0;
    float len2 = vx * vx + vy * vy;
    float tt = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
    if (tt < 0.0f) tt = 0.0f; else if (tt > 1.0f) tt = 1.0f;
    float cx = s->x0 + tt * vx, cy = s->y0 + tt * vy;
    float dx = px - cx, dy = py - cy;
    return sqrtf(dx * dx + dy * dy);
}

/* ------------------------------------------------------------------ */
/* per-pixel occlusion ratio (exhaustive brute force)                 */
/* ------------------------------------------------------------------ */
/* Per-pixel occlusion ratio (exhaustive brute force). Only m->w/m->h are
 * read; the caller owns the output buffer, so m->buf may be NULL. */
int sh_occlusion(const ShadowMap *m, const ShLight *light,
                 const ShSeg *segs, int nseg, float penumbra, float *out) {
    if (!m || m->w <= 0 || m->h <= 0 || !light || !out || (nseg > 0 && !segs))
        return SH_EINVAL;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return SH_ERANGE;
    if (penumbra < 0.0f) return SH_ERANGE;

    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            size_t idx = (size_t)y * (size_t)m->w + (size_t)x;
            int hard = 0;
            float best = penumbra + 1.0f;
            for (int i = 0; i < nseg; ++i) {
                if (blocked(&segs[i], light->x, light->y, px, py)) { hard = 1; break; }
                if (penumbra > 0.0f) {
                    float d = point_seg_dist(px, py, &segs[i]);
                    if (d < best) best = d;
                }
            }
            float o;
            if (hard) o = 1.0f;
            else if (penumbra > 0.0f && best <= penumbra)
                o = 1.0f - best / penumbra; /* soft ramp outside the umbra */
            else o = 0.0f;
            out[idx] = o;
        }
    }
    return SH_OK;
}

/* ------------------------------------------------------------------ */
/* two-stage tinted composite                                         */
/* ------------------------------------------------------------------ */
static void composite(ShFrame *frame, const ShColor *base,
                      const float *occ,
                      double *sum_mix, long *shadow_px) {
    size_t n = (size_t)frame->w * (size_t)frame->h;
    for (size_t i = 0; i < n; ++i) {
        float o = occ[i];
        if (o <= 0.0f) continue;
        /* stage 1: shadow base tint from the light color.
         * base[c] = light[c] * (1 - strength), precomputed by caller.
         * stage 2: blend by occlusion ratio. */
        float lb[3];
        lb[0] = (float)base->r; lb[1] = (float)base->g; lb[2] = (float)base->b;
        for (int c = 0; c < 3; ++c) {
            float v = (float)frame->rgb[i * 3 + c] * (1.0f - o) + lb[c] * o;
            if (v < 0.0f) v = 0.0f; else if (v > 255.0f) v = 255.0f;
            frame->rgb[i * 3 + c] = (unsigned char)(v + 0.5f);
        }
        *sum_mix += (double)o;
        if (o > 0.0f) ++*shadow_px;
    }
}

/* legacy gray path used when the tint switch is off: reproduces
 * sh_render + sh_write_pgm's scalar mapping onto the RGB frame. */
static int composite_legacy_gray(ShFrame *frame, const ShLight *light,
                                 const ShSeg *segs, int nseg) {
    for (int y = 0; y < frame->h; ++y) {
        for (int x = 0; x < frame->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int shadowed = 0;
            for (int i = 0; i < nseg; ++i)
                if (blocked(&segs[i], light->x, light->y, px, py)) { shadowed = 1; break; }
            float v = 0.0f;
            if (!shadowed) {
                float dx = px - light->x, dy = py - light->y;
                float d = sqrtf(dx * dx + dy * dy);
                v = light->intensity / (1.0f + d / light->radius);
            }
            if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
            unsigned char g = (unsigned char)(v * 255.0f + 0.5f);
            size_t idx = ((size_t)y * (size_t)frame->w + (size_t)x) * 3;
            frame->rgb[idx + 0] = g;
            frame->rgb[idx + 1] = g;
            frame->rgb[idx + 2] = g;
        }
    }
    return SH_OK;
}

/* ------------------------------------------------------------------ */
/* fast gear: coarse tiles marked by each segment's exact shadow wedge */
/* ------------------------------------------------------------------ */
typedef struct {
    int cols, rows, cell;
    int *head;     /* head[cell] = index into pairs, -1 if empty       */
    int *next;     /* next[pair] = next pair in the same cell's chain  */
    int *seg_of;   /* seg_of[pair] = segment index                     */
    int  npairs;
    int  cap;
} Grid;

static void grid_free(Grid *g) {
    if (!g) return;
    free(g->head); free(g->next); free(g->seg_of);
    g->head = NULL; g->next = NULL; g->seg_of = NULL;
}

/* 2D cross (b-a) x (c-a), used by the wedge test below. */
static float cross2(float ax, float ay, float bx, float by, float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

/* A segment's hard shadow is the infinite wedge behind it, bounded by the
 * two rays light->p0 and light->p1. A tile must register the segment if
 * any point of the tile can lie in that wedge. This is tested robustly by
 * inflating the wedge by the tile's circumradius (cell * sqrt(2)/2): a
 * tile center inside the inflated wedge means the tile may contain a true
 * shadow point. Half-plane distances give a conservative (over-inclusive,
 * never under-inclusive) answer, so the cull never drops a real occluder. */
static int tile_touches_wedge(const ShSeg *s, const ShLight *L,
                              float x0, float y0, float x1, float y1) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float half = (x1 - x0) * 0.5f;                 /* square tile */
    float radius = half * 1.41421356f;             /* circumradius */

    /* Signed distance numerator of the tile center from each wedge
     * boundary; divide by ray length for a true distance. The shadow
     * wedge lies between rays L->p0 and L->p1 on the far side of the
     * segment; orient each boundary so its "inside" sign is known from
     * where the segment's other endpoint sits. */
    float d0 = cross2(L->x, L->y, s->x0, s->y0, cx, cy);
    float d1 = cross2(L->x, L->y, s->x1, s->y1, cx, cy);
    float inner0 = cross2(L->x, L->y, s->x0, s->y0, s->x1, s->y1);
    float inner1 = cross2(L->x, L->y, s->x1, s->y1, s->x0, s->y0);
    float len0 = hypotf(s->x0 - L->x, s->y0 - L->y);
    float len1 = hypotf(s->x1 - L->x, s->y1 - L->y);
    if (len0 < 1e-9f || len1 < 1e-9f) return 1;    /* degenerate: keep safe */
    int ok0 = (inner0 >= 0.0f) ? (d0 >= -radius * len0)
                               : (d0 <=  radius * len0);
    int ok1 = (inner1 >= 0.0f) ? (d1 >= -radius * len1)
                               : (d1 <=  radius * len1);
    if (!ok0 || !ok1) return 0;

    /* Far-side test: center within radius of the segment's far half-plane.
     * Shadow lies on the opposite side of the segment from the light. */
    float ds = cross2(s->x0, s->y0, s->x1, s->y1, cx, cy);
    float light_side = cross2(s->x0, s->y0, s->x1, s->y1, L->x, L->y);
    float lens = hypotf(s->x1 - s->x0, s->y1 - s->y0);
    if (lens < 1e-9f) return 1;
    int far = (light_side >= 0.0f) ? (ds <= radius * lens)
                                   : (ds >= -radius * lens);
    return far;
}

static int grid_build(Grid *g, const ShSeg *segs, int nseg,
                      int w, int h, const ShLight *light) {
    g->cell = (w < h ? w : h) / 8;
    if (g->cell < 1) g->cell = 1;
    g->cols = (w + g->cell - 1) / g->cell;
    g->rows = (h + g->cell - 1) / g->cell;
    size_t ncells = (size_t)g->cols * (size_t)g->rows;

    /* The wedge test is already conservative over the whole tile (it
     * inflates each half-plane by the tile circumradius), so a tile that
     * can contain any shadow point is registered directly. */
    int total = 0;
    for (int i = 0; i < nseg; ++i)
        for (int r = 0; r < g->rows; ++r)
            for (int c = 0; c < g->cols; ++c) {
                float x0 = c * g->cell, y0 = r * g->cell;
                if (tile_touches_wedge(&segs[i], light, x0, y0,
                                       x0 + g->cell, y0 + g->cell))
                    ++total;
            }

    g->cap = total + 1;
    g->head = (int *)malloc(ncells * sizeof(int));
    g->next = (int *)malloc((size_t)g->cap * sizeof(int));
    g->seg_of = (int *)malloc((size_t)g->cap * sizeof(int));
    if (!g->head || !g->next || !g->seg_of) { grid_free(g); return SH_EINVAL; }
    for (size_t i = 0; i < ncells; ++i) g->head[i] = -1;
    g->npairs = 0;

    for (int i = 0; i < nseg; ++i)
        for (int r = 0; r < g->rows; ++r)
            for (int c = 0; c < g->cols; ++c) {
                float x0 = c * g->cell, y0 = r * g->cell;
                if (!tile_touches_wedge(&segs[i], light, x0, y0,
                                        x0 + g->cell, y0 + g->cell))
                    continue;
                int cell = r * g->cols + c;
                int p = g->npairs++;
                g->seg_of[p] = i;
                g->next[p] = g->head[cell];
                g->head[cell] = p;
            }
    return SH_OK;
}

/* Fast occlusion. Hard shadow: each pixel ray-tests only the segments whose
 * exact shadow wedge covers its tile -- a lossless cull. Soft (penumbra)
 * distance has no safe spatial cull, so the nearest segment there is found
 * by a full scan identical to the brute-force path; both gears therefore
 * produce bit-identical output. */
static int occlusion_fast(ShFrame *frame, const ShLight *light,
                          const ShSeg *segs, int nseg, float penumbra,
                          float *occ, double *sum_occ) {
    Grid g;
    int rc = grid_build(&g, segs, nseg, frame->w, frame->h, light);
    if (rc < 0) return rc;

    for (int y = 0; y < frame->h; ++y) {
        for (int x = 0; x < frame->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int cc = (int)(px / g.cell), rr = (int)(py / g.cell);

            int hard = 0;
            int cell = rr * g.cols + cc;
            for (int p = g.head[cell]; p >= 0 && !hard; p = g.next[p]) {
                int si = g.seg_of[p];
                if (blocked(&segs[si], light->x, light->y, px, py)) hard = 1;
            }

            float o;
            if (hard) {
                o = 1.0f;
            } else if (penumbra > 0.0f) {
                /* soft ramp needs the true nearest segment over the whole
                 * scene; scan every segment so it matches the exhaustive
                 * path bit-for-bit. */
                float best = penumbra + 1.0f;
                for (int i = 0; i < nseg; ++i) {
                    float d = point_seg_dist(px, py, &segs[i]);
                    if (d < best) best = d;
                }
                o = (best <= penumbra) ? 1.0f - best / penumbra : 0.0f;
            } else {
                o = 0.0f;
            }
            size_t idx = (size_t)y * (size_t)frame->w + (size_t)x;
            occ[idx] = o;
            *sum_occ += (double)o;
        }
    }
    grid_free(&g);
    return SH_OK;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

int sh_render_tinted(ShFrame *frame, const ShLight *light,
                     const ShSeg *segs, int nseg,
                     const ShTune *tune, int gear) {
    if (!frame || !frame->rgb || !light || !tune || (nseg > 0 && !segs)) return SH_EINVAL;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return SH_ERANGE;
    if (gear != 0 && gear != 1) return SH_EINVAL;

    /* switch off: legacy gray behavior, frame untouched by tint math */
    if (!tune->enable) return composite_legacy_gray(frame, light, segs, nseg);

    char a[32], b[32];
    int rc = sh_tune_check(tune, a, sizeof a, b, sizeof b);
    if (rc < 0) return rc;
    ShColor lc;
    rc = sh_resolve_color(tune, &lc);
    if (rc < 0) return rc;

    /* stage 1: shadow base tint from light color */
    ShColor base;
    base.r = (unsigned char)((float)lc.r * (1.0f - tune->shadow_strength) + 0.5f);
    base.g = (unsigned char)((float)lc.g * (1.0f - tune->shadow_strength) + 0.5f);
    base.b = (unsigned char)((float)lc.b * (1.0f - tune->shadow_strength) + 0.5f);

    size_t n = (size_t)frame->w * (size_t)frame->h;
    float *occ = (float *)calloc(n, sizeof(float));
    if (!occ) return SH_EINVAL;

    if (gear == 0) {
        ShadowMap mm = {frame->w, frame->h, NULL};
        rc = sh_occlusion(&mm, light, segs, nseg, tune->penumbra, occ);
        if (rc < 0) { free(occ); return rc; }
    } else {
        double dummy = 0.0;
        rc = occlusion_fast(frame, light, segs, nseg, tune->penumbra, occ, &dummy);
        if (rc < 0) { free(occ); return rc; }
    }

    /* stage 2: mix into the picture by occlusion ratio */
    double sum_mix = 0.0;
    long shadow_px = 0;
    composite(frame, &base, occ, &sum_mix, &shadow_px);
    (void)sum_mix; (void)shadow_px;
    free(occ);
    return SH_OK;
}

/* ------------------------------------------------------------------ */
/* color science: sRGB -> Lab and CIE76 delta E                       */
/* ------------------------------------------------------------------ */
static void srgb_to_lab(unsigned char r, unsigned char g, unsigned char b,
                        double *L, double *A, double *B) {
    double lr = r / 255.0, lg = g / 255.0, lb = b / 255.0;
    double lin[3] = {lr, lg, lb};
    for (int i = 0; i < 3; ++i)
        lin[i] = lin[i] <= 0.04045 ? lin[i] / 12.92
                                   : pow((lin[i] + 0.055) / 1.055, 2.4);
    /* sRGB D65 -> XYZ */
    double X = lin[0] * 0.4124564 + lin[1] * 0.3575761 + lin[2] * 0.1804375;
    double Y = lin[0] * 0.2126729 + lin[1] * 0.7151522 + lin[2] * 0.0721750;
    double Z = lin[0] * 0.0193339 + lin[1] * 0.1191920 + lin[2] * 0.9503041;
    static const double xn = 0.95047, yn = 1.0, zn = 1.08883;
    double v[3] = {X / xn, Y / yn, Z / zn};
    for (int i = 0; i < 3; ++i)
        v[i] = v[i] > 0.008856 ? cbrt(v[i]) : (7.787 * v[i] + 16.0 / 116.0);
    *L = 116.0 * v[1] - 16.0;
    *A = 500.0 * (v[0] - v[1]);
    *B = 200.0 * (v[1] - v[2]);
}

static double deltaE76(unsigned char r1, unsigned char g1, unsigned char b1,
                       unsigned char r2, unsigned char g2, unsigned char b2) {
    double L1, A1, B1, L2, A2, B2;
    srgb_to_lab(r1, g1, b1, &L1, &A1, &B1);
    srgb_to_lab(r2, g2, b2, &L2, &A2, &B2);
    double dL = L1 - L2, da = A1 - A2, db = B1 - B2;
    return sqrt(dL * dL + da * da + db * db);
}

/* ------------------------------------------------------------------ */
/* deterministic test scene                                           */
/* ------------------------------------------------------------------ */
static unsigned int rng_state;
static unsigned int xrnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static float frnd(float lo, float hi) {
    return lo + (hi - lo) * (float)(xrnd() & 0xFFFFFF) / (float)0x1000000;
}

int sh_test_scene(int w, int h, int nseg, ShSeg *segs) {
    if (!segs || nseg < 0 || w <= 0 || h <= 0) return SH_EINVAL;
    rng_state = 0x51A7u + ((unsigned int)nseg << 8) +
                ((unsigned int)w << 1) + (unsigned int)h;
    if (rng_state == 0) rng_state = 1;
    for (int i = 0; i < nseg; ++i) {
        segs[i].x0 = frnd(0.0f, (float)w);
        segs[i].y0 = frnd(0.0f, (float)h);
        segs[i].x1 = segs[i].x0 + frnd(-12.0f, 12.0f);
        segs[i].y1 = segs[i].y0 + frnd(-12.0f, 12.0f);
    }
    return SH_OK;
}

/* expected tinted output for one pixel given its occlusion and the base */
static void expected_pixel(unsigned char bg[3], const ShColor *base, float o,
                           unsigned char out[3]) {
    float lb[3] = {(float)base->r, (float)base->g, (float)base->b};
    for (int c = 0; c < 3; ++c) {
        float v = (float)bg[c] * (1.0f - o) + lb[c] * o;
        if (v < 0.0f) v = 0.0f; else if (v > 255.0f) v = 255.0f;
        out[c] = (unsigned char)(v + 0.5f);
    }
}

int sh_bench_run(ShFrame *frame, const ShLight *light, int nseg,
                 const ShTune *tune, int gear, ShBench *out) {
    if (!frame || !frame->rgb || !light || !tune || !out || nseg < 0) return SH_EINVAL;
    ShSeg *segs = (ShSeg *)malloc((size_t)(nseg > 0 ? nseg : 1) * sizeof(ShSeg));
    if (!segs) return SH_EINVAL;
    sh_test_scene(frame->w, frame->h, nseg, segs);

    /* fixed mid-gray backdrop so tint error is well defined */
    sh_frame_fill(frame, 210, 210, 210);
    unsigned char bg[3] = {210, 210, 210};

    ShColor base = {0, 0, 0};
    if (tune->enable) {
        ShColor lc;
        int rc = sh_resolve_color(tune, &lc);
        if (rc < 0) { free(segs); return rc; }
        base.r = (unsigned char)((float)lc.r * (1.0f - tune->shadow_strength) + 0.5f);
        base.g = (unsigned char)((float)lc.g * (1.0f - tune->shadow_strength) + 0.5f);
        base.b = (unsigned char)((float)lc.b * (1.0f - tune->shadow_strength) + 0.5f);
    }

    double t0 = now_ms();
    int rc = sh_render_tinted(frame, light, segs, nseg, tune, gear);
    double t1 = now_ms();
    if (rc < 0) { free(segs); return rc; }

    size_t npix = (size_t)frame->w * (size_t)frame->h;
    ShadowMap mm = {frame->w, frame->h, NULL};
    float *occ = (float *)malloc(npix * sizeof(float));
    if (!occ) { free(segs); return SH_EINVAL; }
    rc = sh_occlusion(&mm, light, segs, nseg,
                      tune->enable ? tune->penumbra : 0.0f, occ);
    if (rc < 0) { free(occ); free(segs); return rc; }

    long shadow_px = 0;
    double sum_occ = 0.0, sum_mix = 0.0, sum_err = 0.0, max_err = 0.0;
    double sum_de = 0.0, max_de = 0.0;
    for (size_t i = 0; i < npix; ++i) {
        float o = occ[i];
        sum_occ += o;
        if (o > 0.0f) ++shadow_px;
        if (tune->enable) {
            sum_mix += o;
            unsigned char exp[3];
            expected_pixel(bg, &base, o, exp);
            for (int c = 0; c < 3; ++c) {
                double e = fabs((double)frame->rgb[i * 3 + c] - (double)exp[c]);
                sum_err += e;
                if (e > max_err) max_err = e;
            }
            /* recolor delta E: tinted shadow vs. the gray shadow the old
             * pipeline would have put in the same pixel */
            ShColor black = {0, 0, 0};
            unsigned char gray[3];
            expected_pixel(bg, &black, o, gray);
            double de = deltaE76(frame->rgb[i * 3], frame->rgb[i * 3 + 1],
                                 frame->rgb[i * 3 + 2], gray[0], gray[1], gray[2]);
            sum_de += de;
            if (de > max_de) max_de = de;
        }
    }

    out->w = frame->w; out->h = frame->h; out->nseg = nseg; out->gear = gear;
    out->pixels = (long)npix;
    out->shadow_pixels = shadow_px;
    out->time_ms = t1 - t0;
    out->mean_occlusion = sum_occ / (double)npix;
    out->mean_mix = tune->enable ? sum_mix / (double)npix : 0.0;
    out->tint_err = tune->enable ? sum_err / ((double)npix * 3.0) : 0.0;
    out->max_tint_err = max_err;
    out->recolor_de = tune->enable ? sum_de / (double)npix : 0.0;
    out->recolor_de_max = max_de;

    free(occ);
    free(segs);
    return SH_OK;
}

/* ------------------------------------------------------------------ */
/* exhaustive cross-check (independent reference implementation)      */
/* ------------------------------------------------------------------ */
int sh_bench_verify_exhaustive(const ShFrame *frame, const ShLight *light,
                               const ShSeg *segs, int nseg,
                               const ShTune *tune, const unsigned char bg_in[3],
                               long *mismatch, double *max_channel_diff) {
    if (!frame || !frame->rgb || !light || !tune || !bg_in ||
        (nseg > 0 && !segs)) return SH_EINVAL;
    if (!tune->enable) { if (mismatch) *mismatch = 0; if (max_channel_diff) *max_channel_diff = 0.0; return SH_OK; }

    ShColor lc;
    int rc = sh_resolve_color(tune, &lc);
    if (rc < 0) return rc;
    ShColor base;
    base.r = (unsigned char)((float)lc.r * (1.0f - tune->shadow_strength) + 0.5f);
    base.g = (unsigned char)((float)lc.g * (1.0f - tune->shadow_strength) + 0.5f);
    base.b = (unsigned char)((float)lc.b * (1.0f - tune->shadow_strength) + 0.5f);

    /* backdrop that was present before the shadow was composited */
    unsigned char bg[3] = {bg_in[0], bg_in[1], bg_in[2]};
    long mm = 0;
    double maxd = 0.0;
    for (int y = 0; y < frame->h; ++y) {
        for (int x = 0; x < frame->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int hard = 0;
            float best = tune->penumbra + 1.0f;
            for (int i = 0; i < nseg; ++i) {
                if (blocked(&segs[i], light->x, light->y, px, py)) { hard = 1; break; }
                if (tune->penumbra > 0.0f) {
                    float d = point_seg_dist(px, py, &segs[i]);
                    if (d < best) best = d;
                }
            }
            float o = hard ? 1.0f
                : (tune->penumbra > 0.0f && best <= tune->penumbra
                       ? 1.0f - best / tune->penumbra : 0.0f);
            unsigned char exp[3];
            expected_pixel(bg, &base, o, exp);
            size_t idx = ((size_t)y * (size_t)frame->w + (size_t)x) * 3;
            for (int c = 0; c < 3; ++c) {
                double d = fabs((double)frame->rgb[idx + c] - (double)exp[c]);
                if (d > maxd) maxd = d;
                if (d > 0.5) ++mm; /* rounding threshold */
            }
        }
    }
    if (mismatch) *mismatch = mm;
    if (max_channel_diff) *max_channel_diff = maxd;
    return SH_OK;
}
