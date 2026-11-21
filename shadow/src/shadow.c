#include "shadow.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sh_init(ShadowMap *m, int w, int h) {
    if (!m || w <= 0 || h <= 0) return -1;
    m->buf = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!m->buf) return -1;
    m->w = w; m->h = h;
    return 0;
}

void sh_free(ShadowMap *m) { if (m) { free(m->buf); m->buf = NULL; m->w = 0; m->h = 0; } }

int sh_clear(ShadowMap *m) {
    if (!m) return -1;
    for (int i = 0; i < m->w * m->h; ++i) m->buf[i] = 0.0f;
    return 0;
}

static int blocked(const ShSeg *s, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}

/* ------------------------------------------------------------------ */
/* Legacy renderer: unchanged, including its exact float behaviour.   */
/* ------------------------------------------------------------------ */
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int shadowed = 0;
            for (int i = 0; i < nseg; ++i)
                if (blocked(&segs[i], light->x, light->y, px, py)) { shadowed = 1; break; }
            float v = 0.0f;
            if (!shadowed) {
                float dx = px - light->x, dy = py - light->y;
                float d = sqrtf(dx * dx + dy * dy);
                float fall = 1.0f / (1.0f + d / light->radius);
                v = light->intensity * fall;
            }
            m->buf[(size_t)y * m->w + x] = v;
        }
    }
    return 0;
}

int sh_write_pgm(const ShadowMap *m, const char *path) {
    if (!m || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    fprintf(f, "P5\n%d %d\n255\n", m->w, m->h);
    for (int i = 0; i < m->w * m->h; ++i) {
        float v = m->buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        unsigned char b = (unsigned char)(v * 255.0f + 0.5f);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return 0;
}

/* ================================================================== */
/* Cast-distance truncation                                           */
/* ================================================================== */

static const char *const SH_FIELD_NAMES[SH_F_COUNT] = {
    "enable", "tier_count", "cutoff",
    "split_0", "split_1", "split_2",
    "bright_0", "bright_1", "bright_2", "bright_3"
};

static const ShType SH_FIELD_TYPES[SH_F_COUNT] = {
    SH_TY_INT, SH_TY_INT, SH_TY_FLOAT,
    SH_TY_FLOAT, SH_TY_FLOAT, SH_TY_FLOAT,
    SH_TY_FLOAT, SH_TY_FLOAT, SH_TY_FLOAT, SH_TY_FLOAT
};

const char *sh_field_name(ShField f) {
    if ((int)f < 0 || (int)f >= SH_F_COUNT) return NULL;
    return SH_FIELD_NAMES[f];
}

ShType sh_field_type(ShField f) {
    if ((int)f < 0 || (int)f >= SH_F_COUNT) return SH_TY_NONE;
    return SH_FIELD_TYPES[f];
}

void sh_error_clear(ShError *err) {
    if (!err) return;
    err->code = SH_OK;
    err->field = NULL;
    err->msg[0] = '\0';
}

static int fail(ShError *err, int code, const char *field, const char *fmt, ...) {
    if (err) {
        va_list ap;
        err->code = code;
        err->field = field;
        va_start(ap, fmt);
        vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
        va_end(ap);
    }
    return code;
}

void sh_cfg_default(ShTruncCfg *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->enable = 0;
    cfg->tier_count = 1;
    cfg->cutoff = 0.0f;
}

static int bright_index(ShField f, int *out) {
    int i;
    if (f >= SH_F_BRIGHT_0 && f <= SH_F_BRIGHT_3) { i = (int)f - (int)SH_F_BRIGHT_0; *out = i; return 1; }
    if (f >= SH_F_SPLIT_0 && f <= SH_F_SPLIT_2) { i = (int)f - (int)SH_F_SPLIT_0; *out = i; return 1; }
    return 0;
}

int sh_cfg_set(ShTruncCfg *cfg, ShField f, ShType ty, const void *value, ShError *err) {
    const char *name = sh_field_name(f);
    ShType want = sh_field_type(f);
    if (err) sh_error_clear(err);
    if (!name) return fail(err, SH_ERR_FIELD, NULL, "unknown field id %d", (int)f);
    if (!cfg) return fail(err, SH_ERR_NULL, name, "NULL config while setting %s", name);
    if (want == SH_TY_NONE) return fail(err, SH_ERR_FIELD, name, "field %s has no type", name);
    if (ty != want)
        return fail(err, SH_ERR_TYPE, name,
                    "field %s is %s, got %s", name,
                    want == SH_TY_INT ? "int" : "float",
                    ty == SH_TY_INT ? "int" : (ty == SH_TY_FLOAT ? "float" : "none"));
    if (!value) return fail(err, SH_ERR_NULL, name, "NULL value for field %s", name);

    int idx = 0;
    if (want == SH_TY_INT) {
        int v = *(const int*)value;
        switch (f) {
        case SH_F_ENABLE:
            if (v != 0 && v != 1)
                return fail(err, SH_ERR_RANGE, name, "field %s must be 0 or 1, got %d", name, v);
            cfg->enable = v;
            return SH_OK;
        case SH_F_TIER_COUNT:
            if (v < 1 || v > SH_MAX_TIERS)
                return fail(err, SH_ERR_RANGE, name, "field %s must be 1..%d, got %d",
                            name, SH_MAX_TIERS, v);
            cfg->tier_count = v;
            return SH_OK;
        default:
            return fail(err, SH_ERR_FIELD, name, "field %s is not an int field", name);
        }
    }

    float v = *(const float*)value;
    if (!isfinite(v)) return fail(err, SH_ERR_RANGE, name, "field %s must be finite", name);
    switch (f) {
    case SH_F_CUTOFF:
        if (v <= 0.0f)
            return fail(err, SH_ERR_RANGE, name, "field %s must be > 0, got %g", name, (double)v);
        cfg->cutoff = v;
        return SH_OK;
    case SH_F_SPLIT_0: case SH_F_SPLIT_1: case SH_F_SPLIT_2:
        bright_index(f, &idx);
        if (v < 0.0f)
            return fail(err, SH_ERR_RANGE, name, "field %s must be >= 0, got %g", name, (double)v);
        cfg->split[idx] = v;
        return SH_OK;
    case SH_F_BRIGHT_0: case SH_F_BRIGHT_1: case SH_F_BRIGHT_2: case SH_F_BRIGHT_3:
        bright_index(f, &idx);
        if (v < 0.0f || v > 1.0f)
            return fail(err, SH_ERR_RANGE, name, "field %s must be in [0,1], got %g", name, (double)v);
        cfg->bright[idx] = v;
        return SH_OK;
    default:
        return fail(err, SH_ERR_FIELD, name, "field %s is not a float field", name);
    }
}

int sh_cfg_validate(const ShTruncCfg *cfg, ShError *err) {
    if (err) sh_error_clear(err);
    if (!cfg) return fail(err, SH_ERR_NULL, NULL, "NULL config");
    if (!cfg->enable) return SH_OK;
    if (cfg->tier_count < 1 || cfg->tier_count > SH_MAX_TIERS)
        return fail(err, SH_ERR_RANGE, "tier_count", "field tier_count must be 1..%d, got %d",
                    SH_MAX_TIERS, cfg->tier_count);
    if (!isfinite(cfg->cutoff) || cfg->cutoff <= 0.0f)
        return fail(err, SH_ERR_RANGE, "cutoff", "field cutoff must be > 0, got %g",
                    (double)cfg->cutoff);
    float prev = 0.0f;
    for (int i = 0; i < cfg->tier_count - 1; ++i) {
        const char *name = SH_FIELD_NAMES[SH_F_SPLIT_0 + i];
        float s = cfg->split[i];
        if (!isfinite(s) || s <= 0.0f)
            return fail(err, SH_ERR_RANGE, name, "field %s must be > 0, got %g", name, (double)s);
        if (s >= cfg->cutoff)
            return fail(err, SH_ERR_ORDER, name,
                        "field %s = %g must be below cutoff %g", name, (double)s, (double)cfg->cutoff);
        if (s <= prev)
            return fail(err, SH_ERR_ORDER, name,
                        "field %s = %g must be above the previous split %g", name, (double)s, (double)prev);
        prev = s;
    }
    for (int i = 0; i < cfg->tier_count; ++i) {
        const char *name = SH_FIELD_NAMES[SH_F_BRIGHT_0 + i];
        float b = cfg->bright[i];
        if (!isfinite(b) || b < 0.0f || b > 1.0f)
            return fail(err, SH_ERR_RANGE, name, "field %s must be in [0,1], got %g", name, (double)b);
    }
    return SH_OK;
}

/* --- geometry ---------------------------------------------------- */

/* Receiver tier: first k with d <= split[k], else tier_count-1. */
static int tier_of_distance(double d, const ShTruncCfg *cfg) {
    for (int k = 0; k < cfg->tier_count - 1; ++k)
        if (d <= (double)cfg->split[k]) return k;
    return cfg->tier_count - 1;
}

/* All t in [0,1] with |A + t*(B-A) - L| == level, appended to ts. */
static int roots_at_level(double ax, double ay, double vx, double vy,
                          double lx, double ly, double level, double ts[2]) {
    double wx = ax - lx, wy = ay - ly;
    double a = vx * vx + vy * vy;
    if (a < 1e-18) return 0;
    double b = wx * vx + wy * vy;
    double c = wx * wx + wy * wy - level * level;
    double disc = b * b - a * c;
    if (disc < 0.0) return 0;
    double r = sqrt(disc);
    double t0 = (-b - r) / a, t1 = (-b + r) / a;
    int n = 0;
    if (t0 > 0.0 && t0 < 1.0) ts[n++] = t0;
    if (t1 > 0.0 && t1 < 1.0 && fabs(t1 - t0) > 1e-15) ts[n++] = t1;
    return n;
}

/* Cut endpoints are placed on the segment and shared between neighbours:
 * the right end of piece i is literally the same float pair as the left end
 * of piece i+1, so the pieces tile the kept part with no gap and no overlap.
 * Retruncating a piece therefore reproduces the same partition instead of
 * manufacturing sliver pieces at every tier edge. */
static int ensure_pieces(ShCastSet *cs, int cap, int *alloc_cap) {
    if (cap <= *alloc_cap) return 0;
    int nc = *alloc_cap ? *alloc_cap * 2 : 16;
    while (nc < cap) nc *= 2;
    ShCastPiece *np = (ShCastPiece*)realloc(cs->pieces, (size_t)nc * sizeof(ShCastPiece));
    if (!np) return -1;
    cs->pieces = np;
    *alloc_cap = nc;
    return 0;
}

void sh_castset_free(ShCastSet *cs) {
    if (!cs) return;
    free(cs->pieces);
    memset(cs, 0, sizeof(*cs));
}

/* Clip one segment to the light-distance bands of cfg and append the pieces.
 *
 * Clipping happens in a frame centred on the light: a point is inside the
 * cutoff exactly when x*x + y*y <= cutoff*cutoff, and the membership test for
 * a piece is taken at its midpoint, away from the rounded ends.  Every
 * breakpoint is a quadratic root at a split level or a kept-interval end, so
 * truncating an already-truncated piece leaves the partition unchanged.
 *
 * Returns 0 on success, -1 on allocation failure. */
static int truncate_segment(ShCastSet *cs, int *alloc_cap,
                            const ShSeg *s, int src, const ShLight *light,
                            const ShTruncCfg *cfg, int *n_kept, int *was_clipped) {
    const double lx = light->x, ly = light->y;
    const double ax = (double)s->x0 - lx, ay = (double)s->y0 - ly;
    const double bx = (double)s->x1 - lx, by = (double)s->y1 - ly;
    const double vx = bx - ax, vy = by - ay;
    const double cutoff = (double)cfg->cutoff;
    const double cut2 = cutoff * cutoff;
    const double a = vx * vx + vy * vy;
    const double b = ax * vx + ay * vy;
    const double c = ax * ax + ay * ay;
    const double inside_eps = 1.0 + 1e-12;

    double lo = 0.0, hi = 1.0;
    if (a < 1e-18) {
        /* Degenerate (zero-length) segment: keep or drop as a point. */
        if (c > cut2 * inside_eps) { *n_kept = 0; *was_clipped = 0; return 0; }
    } else {
        double disc = b * b - a * (c - cut2);
        if (disc < 0.0) { *n_kept = 0; *was_clipped = 0; return 0; }
        double r = sqrt(disc);
        double t0 = (-b - r) / a, t1 = (-b + r) / a;
        if (t0 > lo) lo = t0;
        if (t1 < hi) hi = t1;
        if (hi <= lo) { *n_kept = 0; *was_clipped = 0; return 0; }
    }

    /* Breakpoints: the kept interval's ends plus every split-level crossing. */
    double bp[2 * SH_MAX_TIERS + 4];
    int nb = 0;
    bp[nb++] = lo;
    bp[nb++] = hi;
    for (int k = 0; k < cfg->tier_count - 1; ++k) {
        double ts[2];
        int n = roots_at_level(ax, ay, vx, vy, 0.0, 0.0, (double)cfg->split[k], ts);
        for (int i = 0; i < n; ++i)
            if (ts[i] > lo && ts[i] < hi) bp[nb++] = ts[i];
    }
    for (int i = 1; i < nb; ++i) {           /* insertion sort; nb is tiny */
        double t = bp[i];
        int j = i - 1;
        while (j >= 0 && bp[j] > t) { bp[j + 1] = bp[j]; --j; }
        bp[j + 1] = t;
    }
    /* Coalesce breakpoints that coincide geometrically.  A piece endpoint
     * that sits on a split level is rounded to float, so recomputing the
     * split root lands a hair inside the piece; without this the second
     * truncation would emit a zero-length sliver there.  Only interior
     * breakpoints can be merged this way, and only when there is something
     * to merge with (nb > 2 keeps a degenerate point's two ends apart). */
    if (nb > 2) {
        double rap[2 * SH_MAX_TIERS + 4];
        rap[0] = bp[0];
        int m = 1;
        for (int i = 1; i < nb; ++i) {
            double t = bp[i], tprev = rap[m - 1];
            double p0x = ax + tprev * vx, p0y = ay + tprev * vy;
            double p1x = ax + t * vx, p1y = ay + t * vy;
            double dx = p1x - p0x, dy = p1y - p0y;
            if (sqrt(dx * dx + dy * dy) <= 1e-4) { rap[m - 1] = 0.5 * (tprev + t); }
            else rap[m++] = t;
        }
        for (int i = 0; i < m; ++i) bp[i] = rap[i];
        nb = m;
    }

    int emitted = 0;
    float prev_x = 0.0f, prev_y = 0.0f;   /* right end of the previous piece */
    for (int i = 0; i + 1 < nb; ++i) {
        double t0 = bp[i], t1 = bp[i + 1];
        if (t1 - t0 <= 0.0) continue;
        double tm = 0.5 * (t0 + t1);
        double mx = ax + tm * vx, my = ay + tm * vy;
        if (mx * mx + my * my > cut2 * inside_eps) continue;

        if (ensure_pieces(cs, cs->count + 1, alloc_cap) != 0) return -1;
        ShCastPiece *pc = &cs->pieces[cs->count];
        if (t0 <= 0.0) { pc->seg.x0 = s->x0; pc->seg.y0 = s->y0; }
        else if (emitted > 0) { pc->seg.x0 = prev_x; pc->seg.y0 = prev_y; }
        else { pc->seg.x0 = (float)(lx + ax + t0 * vx); pc->seg.y0 = (float)(ly + ay + t0 * vy); }
        if (t1 >= 1.0) { pc->seg.x1 = s->x1; pc->seg.y1 = s->y1; }
        else { pc->seg.x1 = (float)(lx + ax + t1 * vx); pc->seg.y1 = (float)(ly + ay + t1 * vy); }
        prev_x = pc->seg.x1;
        prev_y = pc->seg.y1;
        pc->src = src;
        pc->tier = tier_of_distance(sqrt((double)mx * mx + (double)my * my), cfg);
        pc->d0 = (float)sqrt((ax + t0 * vx) * (ax + t0 * vx) + (ay + t0 * vy) * (ay + t0 * vy));
        pc->d1 = (float)sqrt((ax + t1 * vx) * (ax + t1 * vx) + (ay + t1 * vy) * (ay + t1 * vy));
        cs->count++;
        cs->per_tier[pc->tier]++;
        cs->len_after += sqrt((double)((pc->seg.x1 - pc->seg.x0) * (pc->seg.x1 - pc->seg.x0) +
                                       (pc->seg.y1 - pc->seg.y0) * (pc->seg.y1 - pc->seg.y0)));
        emitted++;
    }
    *n_kept = emitted;
    *was_clipped = (emitted > 0 && (lo > 0.0 || hi < 1.0));
    return 0;
}

int sh_truncate(ShCastSet *out, const ShLight *light, const ShSeg *segs, int nseg,
                const ShTruncCfg *cfg, ShError *err) {
    if (err) sh_error_clear(err);
    if (!out || !light || (nseg > 0 && !segs)) return fail(err, SH_ERR_NULL, NULL, "NULL argument");
    if (nseg < 0) return fail(err, SH_ERR_RANGE, NULL, "negative segment count %d", nseg);
    if (!cfg) return fail(err, SH_ERR_NULL, NULL, "NULL config");
    if (!cfg->enable) return fail(err, SH_ERR_STATE, "enable", "field enable is 0");
    if (!isfinite(light->x) || !isfinite(light->y))
        return fail(err, SH_ERR_LIGHT, "x", "light position is not finite");
    int rc = sh_cfg_validate(cfg, err);
    if (rc != SH_OK) return rc;

    memset(out, 0, sizeof(*out));
    out->input_count = nseg;
    out->tier_count = cfg->tier_count;
    out->cutoff = cfg->cutoff;
    for (int i = 0; i < SH_MAX_TIERS - 1; ++i) out->split_edge[i] = cfg->split[i];
    for (int i = 0; i < SH_MAX_TIERS; ++i) out->bright[i] = cfg->bright[i];
    for (int i = 0; i < nseg; ++i) {
        double dx = (double)segs[i].x1 - segs[i].x0, dy = (double)segs[i].y1 - segs[i].y0;
        out->len_before += sqrt(dx * dx + dy * dy);
    }

    int alloc_cap = 0;
    for (int i = 0; i < nseg; ++i) {
        int kept = 0, clipped = 0;
        int r = truncate_segment(out, &alloc_cap, &segs[i], i, light, cfg, &kept, &clipped);
        if (r != 0) {
            sh_castset_free(out);
            return fail(err, SH_ERR_MEM, NULL, "allocation failed at segment %d", i);
        }
        if (kept == 0) out->dropped++;
        if (kept > 1) out->split++;
        if (kept > 0 && clipped) out->clipped++;
    }
    if (!out->pieces) {
        out->pieces = (ShCastPiece*)malloc(sizeof(ShCastPiece)); /* count stays 0 */
        if (!out->pieces) return fail(err, SH_ERR_MEM, NULL, "allocation failed");
    }
    return SH_OK;
}

int sh_render_truncated(ShadowMap *m, const ShLight *light, const ShCastSet *cs,
                        const ShTruncCfg *cfg, ShRenderStats *stats, ShError *err) {
    if (err) sh_error_clear(err);
    if (!m || !light || !cs || !cfg) return fail(err, SH_ERR_NULL, NULL, "NULL argument");
    if (!m->buf) return fail(err, SH_ERR_NULL, NULL, "NULL map buffer");
    if (!cfg->enable) return fail(err, SH_ERR_STATE, "enable", "field enable is 0");
    int rc = sh_cfg_validate(cfg, err);
    if (rc != SH_OK) return rc;
    if (cs->tier_count != cfg->tier_count || cs->cutoff != cfg->cutoff)
        return fail(err, SH_ERR_STATE, NULL,
                    "cast set was truncated with tier_count=%d cutoff=%g, config has %d/%g",
                    cs->tier_count, (double)cs->cutoff, cfg->tier_count, (double)cfg->cutoff);
    for (int k = 0; k < cfg->tier_count - 1; ++k)
        if (cs->split_edge[k] != cfg->split[k])
            return fail(err, SH_ERR_STATE, SH_FIELD_NAMES[SH_F_SPLIT_0 + k],
                        "field %s does not match the cast set",
                        SH_FIELD_NAMES[SH_F_SPLIT_0 + k]);
    if (light->radius <= 0.0f || light->intensity < 0.0f || !isfinite(light->x) ||
        !isfinite(light->y) || !isfinite(light->radius) || !isfinite(light->intensity))
        return fail(err, SH_ERR_LIGHT, "radius", "invalid light (radius/intensity/position)");

    const float lx = light->x, ly = light->y;
    const double cutoff = (double)cfg->cutoff;

    if (stats) {
        memset(stats, 0, sizeof(*stats));
        stats->pixels_total = (long)m->w * (long)m->h;
    }
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float dx = px - lx, dy = py - ly;
            float d = sqrtf(dx * dx + dy * dy);
            int beyond = (double)d > cutoff;
            int shadowed = 0;
            if (!beyond) {
                for (int i = 0; i < cs->count; ++i) {
                    if (blocked(&cs->pieces[i].seg, lx, ly, px, py)) { shadowed = 1; break; }
                }
            }
            float fall = 1.0f / (1.0f + d / light->radius);
            float lit = light->intensity * fall;
            int tier = tier_of_distance((double)d, cfg);
            float v = lit;
            if (shadowed) v = lit * cfg->bright[tier];
            m->buf[(size_t)y * m->w + x] = v;

            if (stats) {
                stats->pixels[tier]++;
                stats->lum_sum[tier] += (double)v;
                stats->shadowed[tier] += shadowed;
                stats->lum_sum_total += (double)v;
                stats->shadowed_total += shadowed;
                if (beyond) stats->beyond_cutoff++;
                stats->piece_tests += beyond ? 0 : cs->count;
            }
        }
    }
    return SH_OK;
}

int sh_render_cast(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                   const ShTruncCfg *cfg, ShCastSet *cast_out, ShRenderStats *stats,
                   ShError *err) {
    if (err) sh_error_clear(err);
    if (!m || !light || !cfg || (nseg > 0 && !segs)) return fail(err, SH_ERR_NULL, NULL, "NULL argument");
    if (cast_out) memset(cast_out, 0, sizeof(*cast_out));
    if (!cfg->enable) {
        /* Disabled: exactly the legacy path, no extra work, no extra state. */
        if (stats) memset(stats, 0, sizeof(*stats));
        int rc = sh_render(m, light, segs, nseg);
        if (rc != 0 && err) {
            if (rc == -1) fail(err, SH_ERR_NULL, NULL, "NULL argument");
            else fail(err, SH_ERR_LIGHT, "radius", "invalid light (radius/intensity)");
        }
        return rc;
    }
    int rc = sh_cfg_validate(cfg, err);
    if (rc != SH_OK) return rc;
    if (light->radius <= 0.0f || light->intensity < 0.0f || !isfinite(light->x) ||
        !isfinite(light->y) || !isfinite(light->radius) || !isfinite(light->intensity))
        return fail(err, SH_ERR_LIGHT, "radius", "invalid light (radius/intensity/position)");
    if (nseg < 0) return fail(err, SH_ERR_RANGE, NULL, "negative segment count %d", nseg);

    ShCastSet local;
    ShCastSet *cs = cast_out ? cast_out : &local;
    if (!cast_out) memset(cs, 0, sizeof(*cs));
    rc = sh_truncate(cs, light, segs, nseg, cfg, err);
    if (rc != SH_OK) return rc;
    rc = sh_render_truncated(m, light, cs, cfg, stats, err);
    if (!cast_out) sh_castset_free(cs);
    return rc;
}
