#include "atlas_ref.h"

#if ATLAS_USE_REFCOUNT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int used;
    int refs;
    int rect_id;
    int w, h;          /* requested size, 1x units */
    int x, y;          /* block origin, points */
    int side;          /* block side, points */
    int level;         /* cell<<level == side */
    int page;
    int idle;          /* phase-1 reset mark; cleared on re-acquire */
    unsigned long gen;
} AtlasRefEntry;

struct AtlasRefAtlas {
    AtlasRefConfig cfg;
    int scale;
    int cells;                 /* cells per page edge */
    unsigned char *occ;        /* max_pages * cells * cells */
    unsigned char *pages[ATLAS_REF_MAX_PAGES];
    AtlasRefEntry *index;
    int *id_slot;
    unsigned long next_gen;
    AtlasRefStats st;
};

static int fail_err(AtlasError *e, AtlasStatus code, const char *what,
                    int index, int page, int x, int y, int detail) {
    if (e) {
        e->code = code;
        e->what = what;
        e->index = index;
        e->page = page;
        e->x = x;
        e->y = y;
        e->detail = detail;
    }
    return (int)code;
}

static void clear_err(AtlasError *e) {
    if (!e) return;
    e->code = ATLAS_OK;
    e->what = "ok";
    e->index = -1;
    e->page = -1;
    e->x = -1;
    e->y = -1;
    e->detail = 0;
}

static int is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

static int block_side_points(int need) {
    int side = ATLAS_REF_CELL;
    while (side < need) side <<= 1;
    return side;
}

static size_t occ_index(const AtlasRefAtlas *a, int page, int cx, int cy) {
    return ((size_t)page * (size_t)a->cells + (size_t)cy) * (size_t)a->cells + (size_t)cx;
}

static void recount(AtlasRefAtlas *a) {
    AtlasRefStats *s = &a->st;
    int pages = 0, len = 0, live = 0, idle = 0, refs = 0;
    int used = 0, requested = 0;
    for (int i = 0; i < a->cfg.max_rects; ++i) {
        const AtlasRefEntry *e = &a->index[i];
        if (!e->used) continue;
        ++len;
        if (e->refs > 0) ++live; else ++idle;
        refs += e->refs;
        used += e->side * e->side;
        requested += e->w * e->h * a->scale * a->scale;
        if (e->page + 1 > pages) pages = e->page + 1;
    }
    s->scale = a->scale;
    s->page_points = a->cfg.page_points;
    s->page_px = a->cfg.page_points * a->scale;
    s->page_bytes = 3 * s->page_px * s->page_px;
    s->cell_points = ATLAS_REF_CELL;
    s->pages = pages;
    s->index_len = len;
    s->index_cap = a->cfg.max_rects;
    s->live_blocks = live;
    s->idle_blocks = idle;
    s->live_refs = refs;
    s->used_points = used;
    s->requested_points = requested;
    s->padding_points = used - requested;
    s->stat_error = s->padding_points;
    s->budget_points = a->cfg.max_pages * a->cfg.page_points * a->cfg.page_points;
    s->free_points = s->budget_points - used;
    s->page_total_points = pages * a->cfg.page_points * a->cfg.page_points;
    s->slack_points = s->page_total_points - used;             /* internal waste */
    s->balance_points = used + s->free_points - s->budget_points; /* must be 0 */
    s->budget_bytes = s->budget_points * 3 * a->scale * a->scale;
    s->used_bytes = used * 3 * a->scale * a->scale;
    s->free_bytes = s->free_points * 3 * a->scale * a->scale;
    s->balance_bytes = s->used_bytes + s->free_bytes - s->budget_bytes;
    s->total_bytes = pages * s->page_bytes;
}

AtlasRefAtlas *atlas_ref_create(const AtlasRefConfig *cfg, int scale, AtlasError *err) {
    clear_err(err);
    if (!cfg) { fail_err(err, ATLAS_ERR_ARG, "config is null", -1, -1, -1, -1, 0); return NULL; }
    if (scale != 1 && scale != 2) {
        fail_err(err, ATLAS_ERR_CONFIG, "scale must be 1 or 2", -1, -1, -1, -1, scale);
        return NULL;
    }
    if (!is_pow2(cfg->page_points) || cfg->page_points < ATLAS_REF_CELL ||
        cfg->page_points > ATLAS_REF_MAX_POINTS) {
        fail_err(err, ATLAS_ERR_CONFIG, "page_points must be pow2 in [8,256]",
                 -1, -1, -1, -1, cfg->page_points);
        return NULL;
    }
    if (cfg->max_pages < 1 || cfg->max_pages > ATLAS_REF_MAX_PAGES) {
        fail_err(err, ATLAS_ERR_CONFIG, "max_pages out of range", -1, -1, -1, -1, cfg->max_pages);
        return NULL;
    }
    if (cfg->max_rects < 1 || cfg->max_rects > ATLAS_REF_MAX_RECTS) {
        fail_err(err, ATLAS_ERR_CONFIG, "max_rects out of range", -1, -1, -1, -1, cfg->max_rects);
        return NULL;
    }
    AtlasRefAtlas *a = (AtlasRefAtlas*)calloc(1, sizeof(*a));
    if (!a) { fail_err(err, ATLAS_ERR_NOMEM, "atlas alloc failed", -1, -1, -1, -1, 0); return NULL; }
    a->cfg = *cfg;
    a->scale = scale;
    a->cells = cfg->page_points / ATLAS_REF_CELL;
    a->index = (AtlasRefEntry*)calloc((size_t)cfg->max_rects, sizeof(AtlasRefEntry));
    a->id_slot = (int*)malloc(sizeof(int) * (size_t)cfg->max_rects);
    a->occ = (unsigned char*)calloc((size_t)cfg->max_pages * (size_t)a->cells * (size_t)a->cells, 1);
    if (!a->index || !a->id_slot || !a->occ) {
        atlas_ref_destroy(a);
        fail_err(err, ATLAS_ERR_NOMEM, "atlas table alloc failed", -1, -1, -1, -1, 0);
        return NULL;
    }
    for (int i = 0; i < cfg->max_rects; ++i) a->id_slot[i] = -1;
    recount(a);
    return a;
}

void atlas_ref_destroy(AtlasRefAtlas *a) {
    if (!a) return;
    for (int p = 0; p < ATLAS_REF_MAX_PAGES; ++p) free(a->pages[p]);
    free(a->occ);
    free(a->index);
    free(a->id_slot);
    free(a);
}

static void fill_pos(const AtlasRefAtlas *a, const AtlasRefEntry *e, AtlasRefPos *pos) {
    pos->x = e->x;
    pos->y = e->y;
    pos->w = e->w;
    pos->h = e->h;
    pos->block_id = (int)(e - a->index);
    pos->page = e->page;
}

int atlas_ref_acquire(AtlasRefAtlas *a, int rect_id, int w, int h,
                      AtlasRefPos *pos, AtlasError *err) {
    clear_err(err);
    if (!a || !pos) return fail_err(err, ATLAS_ERR_ARG, "null argument", rect_id, -1, -1, -1, 0);
    if (rect_id < 0 || rect_id >= a->cfg.max_rects)
        return fail_err(err, ATLAS_ERR_BAD_RECT, "rect id out of range", rect_id, -1, -1, -1,
                        a->cfg.max_rects);
    if (w <= 0 || h <= 0)
        return fail_err(err, ATLAS_ERR_BAD_RECT, "non-positive sprite size", rect_id, -1, -1, -1, 0);

    int slot = a->id_slot[rect_id];
    if (slot >= 0) {
        AtlasRefEntry *e = &a->index[slot];
        if (e->w != w || e->h != h) {
            return fail_err(err, ATLAS_ERR_CONFIG, "size mismatch on re-acquire",
                            rect_id, e->page, e->x, e->y, e->w);
        }
        ++e->refs;
        e->idle = 0;
        ++a->st.live_refs;
        ++a->st.refs_total;
        ++a->st.reacquired;
        e->gen = ++a->next_gen;
        fill_pos(a, e, pos);
        recount(a);
        return ATLAS_OK;
    }

    int need = (w > h ? w : h) * a->scale;
    if (need > a->cfg.page_points)
        return fail_err(err, ATLAS_ERR_SIZE, "sprite larger than a page",
                        rect_id, -1, -1, -1, a->cfg.page_points);

    int side = block_side_points(need);
    int cs = side / ATLAS_REF_CELL;
    int nc = a->cells;
    int found_page = -1, found_cx = -1, found_cy = -1;
    for (int p = 0; p < a->cfg.max_pages && found_page < 0; ++p) {
        for (int cy = 0; cy + cs <= nc && found_page < 0; ++cy) {
            for (int cx = 0; cx + cs <= nc; ++cx) {
                int free_block = 1;
                for (int dy = 0; dy < cs && free_block; ++dy)
                    for (int dx = 0; dx < cs; ++dx)
                        if (a->occ[occ_index(a, p, cx + dx, cy + dy)]) { free_block = 0; break; }
                if (free_block) {
                    found_page = p; found_cx = cx; found_cy = cy;
                    break;
                }
            }
        }
    }

    int new_slot = -1;
    for (int i = 0; i < a->cfg.max_rects; ++i)
        if (!a->index[i].used) { new_slot = i; break; }

    if (found_page < 0) {
        if (new_slot < 0)
            return fail_err(err, ATLAS_ERR_BUDGET, "index full",
                            rect_id, -1, -1, -1, a->cfg.max_rects);
        if (a->st.pages >= a->cfg.max_pages)
            return fail_err(err, ATLAS_ERR_FRAGMENTED, "no free block on any page",
                            rect_id, -1, -1, -1, side);
        return fail_err(err, ATLAS_ERR_BUDGET, "block budget exhausted",
                        rect_id, -1, -1, -1, side);
    }
    if (new_slot < 0)
        return fail_err(err, ATLAS_ERR_BUDGET, "index full",
                        rect_id, found_page, found_cx * ATLAS_REF_CELL,
                        found_cy * ATLAS_REF_CELL, a->cfg.max_rects);

    for (int dy = 0; dy < cs; ++dy)
        for (int dx = 0; dx < cs; ++dx)
            a->occ[occ_index(a, found_page, found_cx + dx, found_cy + dy)] = 1;

    AtlasRefEntry *e = &a->index[new_slot];
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->refs = 1;
    e->rect_id = rect_id;
    e->w = w;
    e->h = h;
    e->x = found_cx * ATLAS_REF_CELL;
    e->y = found_cy * ATLAS_REF_CELL;
    e->side = side;
    e->level = 0;
    while ((ATLAS_REF_CELL << e->level) < side) ++e->level;
    e->page = found_page;
    e->gen = ++a->next_gen;
    a->id_slot[rect_id] = new_slot;

    ++a->st.acquired;
    ++a->st.refs_total;
    fill_pos(a, e, pos);
    recount(a);
    return ATLAS_OK;
}

int atlas_ref_release(AtlasRefAtlas *a, int rect_id, AtlasError *err) {
    clear_err(err);
    if (!a) return fail_err(err, ATLAS_ERR_ARG, "null atlas", rect_id, -1, -1, -1, 0);
    if (rect_id < 0 || rect_id >= a->cfg.max_rects)
        return fail_err(err, ATLAS_ERR_BAD_RECT, "rect id out of range", rect_id, -1, -1, -1,
                        a->cfg.max_rects);
    int slot = a->id_slot[rect_id];
    if (slot < 0)
        return fail_err(err, ATLAS_ERR_STALE, "rect has no live block", rect_id, -1, -1, -1, 0);
    AtlasRefEntry *e = &a->index[slot];
    if (e->refs <= 0)
        return fail_err(err, ATLAS_ERR_STALE, "reference count already zero",
                        rect_id, e->page, e->x, e->y, 0);
    --e->refs;
    --a->st.live_refs;
    ++a->st.released;
    if (e->refs == 0) ++a->st.release_zeroed;
    recount(a);
    return ATLAS_OK;
}

int atlas_ref_sample(AtlasRefAtlas *a, int rect_id, AtlasRefPos *pos, AtlasError *err) {
    clear_err(err);
    if (!a || !pos) return fail_err(err, ATLAS_ERR_ARG, "null argument", rect_id, -1, -1, -1, 0);
    if (rect_id < 0 || rect_id >= a->cfg.max_rects)
        return fail_err(err, ATLAS_ERR_BAD_RECT, "rect id out of range", rect_id, -1, -1, -1,
                        a->cfg.max_rects);
    int slot = a->id_slot[rect_id];
    ++a->st.samples;
    if (slot < 0) {
        ++a->st.sample_misses;
        return fail_err(err, ATLAS_ERR_STALE, "rect has no block", rect_id, -1, -1, -1, 0);
    }
    const AtlasRefEntry *e = &a->index[slot];
    if (e->refs > 0) ++a->st.sample_hits;
    else ++a->st.sample_misses;
    fill_pos(a, e, pos);
    return ATLAS_OK;
}

int atlas_ref_reset(AtlasRefAtlas *a, AtlasRefStats *out, AtlasError *err) {
    clear_err(err);
    if (!a) return fail_err(err, ATLAS_ERR_ARG, "null atlas", -1, -1, -1, -1, 0);

    /* phase 1: mark idle blocks by reference count, before touching anything.
     * The mark is sticky until the block is re-acquired. */
    int marked = 0, leaked = 0;
    for (int i = 0; i < a->cfg.max_rects; ++i) {
        AtlasRefEntry *e = &a->index[i];
        if (!e->used) continue;
        if (e->refs == 0) { e->idle = 1; ++marked; }
        else ++leaked;
    }

    /* phase 2: whole-block reset of every page buffer, then clear the index */
    for (int p = 0; p < ATLAS_REF_MAX_PAGES; ++p)
        if (a->pages[p])
            memset(a->pages[p], 0, (size_t)a->st.page_bytes);
    memset(a->occ, 0, (size_t)a->cfg.max_pages * (size_t)a->cells * (size_t)a->cells);
    memset(a->index, 0, sizeof(AtlasRefEntry) * (size_t)a->cfg.max_rects);
    for (int i = 0; i < a->cfg.max_rects; ++i) a->id_slot[i] = -1;

    a->st.reset_marked += marked;
    a->st.reset_leaked += leaked;
    a->st.reset_index_cleared += a->st.index_len;
    ++a->st.resets;
    recount(a);
    if (out) *out = a->st;
    return ATLAS_OK;
}

int atlas_ref_probe(AtlasRefAtlas *a, int page, int x, int y, AtlasError *err) {
    clear_err(err);
    if (!a) return fail_err(err, ATLAS_ERR_ARG, "null atlas", -1, page, x, y, 0);
    if (page < 0 || page >= a->cfg.max_pages ||
        x < 0 || x >= a->cfg.page_points ||
        y < 0 || y >= a->cfg.page_points) {
        ++a->st.oob_checks;
        return fail_err(err, ATLAS_ERR_OOB, "point outside atlas bounds", -1, page, x, y,
                        a->cfg.page_points);
    }
    int cx = x / ATLAS_REF_CELL, cy = y / ATLAS_REF_CELL;
    if (!a->occ[occ_index(a, page, cx, cy)]) return -1;
    for (int i = 0; i < a->cfg.max_rects; ++i) {
        const AtlasRefEntry *e = &a->index[i];
        if (!e->used || e->page != page) continue;
        if (x >= e->x && x < e->x + e->side && y >= e->y && y < e->y + e->side)
            return e->rect_id;
    }
    return -1;
}

int atlas_ref_validate(const AtlasRefAtlas *a, AtlasError *err) {
    clear_err(err);
    if (!a) return fail_err(err, ATLAS_ERR_ARG, "null atlas", -1, -1, -1, -1, 0);
    if (a->scale != 1 && a->scale != 2)
        return fail_err(err, ATLAS_ERR_CONFIG, "bad scale", -1, -1, -1, -1, a->scale);
    if (a->cells != a->cfg.page_points / ATLAS_REF_CELL)
        return fail_err(err, ATLAS_ERR_CONFIG, "cell grid mismatch", -1, -1, -1, -1, a->cells);
    if (a->st.page_px != a->cfg.page_points * a->scale ||
        a->st.page_bytes != 3 * a->st.page_px * a->st.page_px ||
        a->st.budget_points != a->cfg.max_pages * a->cfg.page_points * a->cfg.page_points ||
        a->st.index_cap != a->cfg.max_rects)
        return fail_err(err, ATLAS_ERR_CONFIG, "header geometry mismatch", -1, -1, -1, -1, 0);

    unsigned char *seen = (unsigned char*)calloc((size_t)a->cfg.max_rects, 1);
    unsigned char *refs = (unsigned char*)calloc((size_t)a->cfg.max_pages * (size_t)a->cells * (size_t)a->cells, 1);
    int rc = ATLAS_OK;
    if (!seen || !refs) {
        rc = fail_err(err, ATLAS_ERR_NOMEM, "validate alloc failed", -1, -1, -1, -1, 0);
        goto done;
    }
    for (int i = 0; i < a->cfg.max_rects; ++i) {
        const AtlasRefEntry *e = &a->index[i];
        if (!e->used) continue;
        if (e->refs < 0) {
            rc = fail_err(err, ATLAS_ERR_CONFIG, "negative reference count", i, e->page, e->x, e->y, e->refs);
            goto done;
        }
        if (e->idle && e->refs != 0) {
            rc = fail_err(err, ATLAS_ERR_CONFIG, "live block carries an idle mark", i, e->page, e->x, e->y, e->refs);
            goto done;
        }
        if (e->w <= 0 || e->h <= 0 || e->side < ATLAS_REF_CELL ||
            e->side != (ATLAS_REF_CELL << e->level) ||
            e->side < e->w * a->scale || e->side < e->h * a->scale) {
            rc = fail_err(err, ATLAS_ERR_CONFIG, "inconsistent block size", i, e->page, e->x, e->y, e->side);
            goto done;
        }
        if (e->page < 0 || e->page >= a->cfg.max_pages) {
            rc = fail_err(err, ATLAS_ERR_OOB, "block page out of range", i, e->page, e->x, e->y,
                          a->cfg.max_pages);
            goto done;
        }
        if (e->x < 0 || e->y < 0 ||
            e->x % ATLAS_REF_CELL || e->y % ATLAS_REF_CELL ||
            e->x + e->side > a->cfg.page_points ||
            e->y + e->side > a->cfg.page_points) {
            rc = fail_err(err, ATLAS_ERR_OOB, "block outside page bounds", i, e->page, e->x, e->y,
                          a->cfg.page_points);
            goto done;
        }
        if (e->rect_id < 0 || e->rect_id >= a->cfg.max_rects ||
            a->id_slot[e->rect_id] != i || seen[e->rect_id]) {
            rc = fail_err(err, ATLAS_ERR_CONFIG, "rect id map broken", i, e->page, e->x, e->y, e->rect_id);
            goto done;
        }
        seen[e->rect_id] = 1;
        int cs = e->side / ATLAS_REF_CELL;
        for (int dy = 0; dy < cs; ++dy)
            for (int dx = 0; dx < cs; ++dx) {
                int cx = e->x / ATLAS_REF_CELL + dx, cy = e->y / ATLAS_REF_CELL + dy;
                unsigned char *cell = &refs[occ_index(a, e->page, cx, cy)];
                if (*cell) {
                    rc = fail_err(err, ATLAS_ERR_CONFIG, "blocks overlap", i, e->page, e->x, e->y, 0);
                    goto done;
                }
                *cell = 1;
            }
    }
    for (int i = 0; i < a->cfg.max_rects; ++i) {
        if (a->id_slot[i] >= 0) {
            if (a->id_slot[i] >= a->cfg.max_rects || !a->index[a->id_slot[i]].used ||
                a->index[a->id_slot[i]].rect_id != i) {
                rc = fail_err(err, ATLAS_ERR_CONFIG, "stale id map entry", i, -1, -1, -1,
                              a->id_slot[i]);
                goto done;
            }
        }
    }
    if (memcmp(refs, a->occ, (size_t)a->cfg.max_pages * (size_t)a->cells * (size_t)a->cells) != 0) {
        rc = fail_err(err, ATLAS_ERR_CONFIG, "occupancy grid disagrees with index", -1, -1, -1, -1, 0);
        goto done;
    }

    {
        AtlasRefStats fresh;
        AtlasRefAtlas *m = (AtlasRefAtlas*)a;
        AtlasRefStats saved = m->st;
        recount(m);
        fresh = m->st;
        m->st = saved;
        if (fresh.index_len != a->st.index_len)
            rc = fail_err(err, ATLAS_ERR_CONFIG, "index_len mismatch", -1, -1, -1, -1, fresh.index_len);
        else if (fresh.live_blocks != a->st.live_blocks)
            rc = fail_err(err, ATLAS_ERR_CONFIG, "live_blocks mismatch", -1, -1, -1, -1, fresh.live_blocks);
        else if (fresh.idle_blocks != a->st.idle_blocks)
            rc = fail_err(err, ATLAS_ERR_CONFIG, "idle_blocks mismatch", -1, -1, -1, -1, fresh.idle_blocks);
        else if (fresh.live_refs != a->st.live_refs)
            rc = fail_err(err, ATLAS_ERR_CONFIG, "live_refs mismatch", -1, -1, -1, -1, fresh.live_refs);
        else if (fresh.pages != a->st.pages)
            rc = fail_err(err, ATLAS_ERR_CONFIG, "pages mismatch", -1, -1, -1, -1, fresh.pages);
        else if (fresh.used_points != a->st.used_points)
            rc = fail_err(err, ATLAS_ERR_CONFIG, "used_points mismatch", -1, -1, -1, -1, fresh.used_points);
        else if (fresh.requested_points != a->st.requested_points)
            rc = fail_err(err, ATLAS_ERR_CONFIG, "requested_points mismatch", -1, -1, -1, -1, fresh.requested_points);
        else if (fresh.padding_points != a->st.padding_points)
            rc = fail_err(err, ATLAS_ERR_CONFIG, "padding_points mismatch", -1, -1, -1, -1, fresh.padding_points);
        else if (fresh.free_points != a->st.free_points)
            rc = fail_err(err, ATLAS_ERR_CONFIG, "free_points mismatch", -1, -1, -1, -1, fresh.free_points);
        else if (fresh.balance_points != 0)
            rc = fail_err(err, ATLAS_ERR_CONFIG, "used+free != budget", -1, -1, -1, -1, fresh.balance_points);
        else if (fresh.budget_bytes != a->st.budget_bytes ||
                 fresh.used_bytes != a->st.used_bytes ||
                 fresh.free_bytes != a->st.free_bytes ||
                 fresh.balance_bytes != 0 ||
                 fresh.total_bytes != a->st.total_bytes)
            rc = fail_err(err, ATLAS_ERR_CONFIG, "byte totals mismatch", -1, -1, -1, -1, 0);
    }
done:
    free(seen);
    free(refs);
    return rc;
}

int atlas_ref_stats(const AtlasRefAtlas *a, AtlasRefStats *out) {
    if (!a || !out) return ATLAS_ERR_ARG;
    *out = a->st;
    return ATLAS_OK;
}

int atlas_ref_upload(AtlasRefAtlas *a, int page, const unsigned char *rgb,
                     size_t bytes, AtlasError *err) {
    clear_err(err);
    if (!a || !rgb) return fail_err(err, ATLAS_ERR_ARG, "null argument", -1, page, -1, -1, 0);
    if (page < 0 || page >= a->cfg.max_pages) {
        ++a->st.oob_checks;
        return fail_err(err, ATLAS_ERR_OOB, "page out of range", -1, page, -1, -1, a->cfg.max_pages);
    }
    if (bytes != (size_t)a->st.page_bytes)
        return fail_err(err, ATLAS_ERR_SIZE, "upload size mismatch", -1, page, -1, -1, a->st.page_bytes);
    if (!a->pages[page]) {
        a->pages[page] = (unsigned char*)malloc((size_t)a->st.page_bytes);
        if (!a->pages[page]) return fail_err(err, ATLAS_ERR_NOMEM, "page alloc failed", -1, page, -1, -1, 0);
    }
    memcpy(a->pages[page], rgb, bytes);
    ++a->st.uploads;
    return ATLAS_OK;
}

const unsigned char *atlas_ref_page_pixels(const AtlasRefAtlas *a, int page,
                                           size_t *bytes, AtlasError *err) {
    clear_err(err);
    if (!a || !bytes) {
        fail_err(err, ATLAS_ERR_ARG, "null argument", -1, page, -1, -1, 0);
        return NULL;
    }
    if (page < 0 || page >= a->cfg.max_pages) {
        fail_err(err, ATLAS_ERR_OOB, "page out of range", -1, page, -1, -1, a->cfg.max_pages);
        return NULL;
    }
    if (!a->pages[page]) {
        fail_err(err, ATLAS_ERR_STALE, "page has no backing buffer", -1, page, -1, -1, 0);
        return NULL;
    }
    *bytes = (size_t)a->st.page_bytes;
    return a->pages[page];
}

int atlas_ref_export(const AtlasRefAtlas *a, const char *path, AtlasError *err) {
    clear_err(err);
    if (!a || !path) return fail_err(err, ATLAS_ERR_ARG, "null argument", -1, -1, -1, -1, 0);
    FILE *f = fopen(path, "wb");
    if (!f) return fail_err(err, ATLAS_ERR_IO, "cannot open export file", -1, -1, -1, -1, 0);
    fprintf(f, "atlas scale=%d cell=%d page_points=%d page_px=%d page_bytes=%d\n",
            a->st.scale, a->st.cell_points, a->st.page_points, a->st.page_px, a->st.page_bytes);
    fprintf(f, "pages=%d budget_points=%d used_points=%d free_points=%d index_len=%d live=%d idle=%d\n",
            a->st.pages, a->st.budget_points, a->st.used_points, a->st.free_points,
            a->st.index_len, a->st.live_blocks, a->st.idle_blocks);
    fprintf(f, "rects\n");
    for (int i = 0; i < a->cfg.max_rects; ++i) {
        const AtlasRefEntry *e = &a->index[i];
        if (!e->used) continue;
        fprintf(f, "rect %d page=%d x=%d y=%d w=%d h=%d block=%d refs=%d\n",
                i, e->page, e->x, e->y, e->w, e->h, e->side, e->refs);
    }
    fprintf(f, "pages\n");
    for (int p = 0; p < a->cfg.max_pages; ++p) {
        int n = 0, area = 0;
        for (int i = 0; i < a->cfg.max_rects; ++i) {
            const AtlasRefEntry *e = &a->index[i];
            if (!e->used || e->page != p) continue;
            ++n;
            area += e->side * e->side;
        }
        if (n) fprintf(f, "page %d blocks=%d used_points=%d\n", p, n, area);
    }
    int rc = ferror(f) ? ATLAS_ERR_IO : ATLAS_OK;
    if (fclose(f) != 0) rc = ATLAS_ERR_IO;
    if (rc != ATLAS_OK) return fail_err(err, ATLAS_ERR_IO, "write failed", -1, -1, -1, -1, 0);
    return ATLAS_OK;
}

#endif /* ATLAS_USE_REFCOUNT */
