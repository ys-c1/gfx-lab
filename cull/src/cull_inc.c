#include "cull_inc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int overlaps(CullBox b, CullView v) {
    return !(b.x + b.w < v.x || b.x > v.x + v.w ||
             b.y + b.h < v.y || b.y > v.y + v.h);
}

/* 错误信息缓冲：返回给调用方的 err 指针指向这里，下次调用覆盖。
 * 单线程使用足够；明确指出出错的字段与原因。 */
static char g_err[128];
static const char *err_negative(const char *role, char axis, double val) {
    snprintf(g_err, sizeof g_err, "%s.%c = %g (negative size)", role, axis, val);
    return g_err;
}
static const char *err_empty(const char *role, double w, double h) {
    snprintf(g_err, sizeof g_err, "%s empty (w=%g, h=%g)", role, w, h);
    return g_err;
}
static const char *err_box(int id, char axis, double val) {
    snprintf(g_err, sizeof g_err, "boxes[%d].%c = %g (negative size)", id, axis, val);
    return g_err;
}

struct CullInc {
    const CullBox *boxes;
    int n;
    CullView view;
    int *visible;          /* 升序、去重的可见 id */
    int nvis;
    /* 均匀网格空间索引 */
    double ox, oy, cw, ch;
    int gw, gh;
    int **cells;
    int *cell_n, *cell_cap;
    /* 候选去重与标记 */
    int *mark;             /* mark[id] == gen 表示已在候选集 */
    unsigned gen;
    char *removed;         /* removed[id]=1 表示本帧退出 */
    int *cand;
    int ncand, capcand;
    int *adds;
    int nadds, capadds;
};

static int vis_contains(const CullInc *st, int id) {
    int lo = 0, hi = st->nvis;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (st->visible[mid] < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < st->nvis && st->visible[lo] == id;
}

/* 视口校验：空视口(w/h 为 0)或负尺寸都报错，err 指出具体字段与原因。
 * role 为该视口的角色名（"view" / "old_view" / "new_view"）。 */
static int valid_view(CullView v, const char *role, const char **err) {
    if (v.w < 0) { if (err) *err = err_negative(role, 'w', v.w); return 0; }
    if (v.h < 0) { if (err) *err = err_negative(role, 'h', v.h); return 0; }
    if (v.w == 0 || v.h == 0) { if (err) *err = err_empty(role, v.w, v.h); return 0; }
    return 1;
}

/* 计算 A\B 的四块矩形分解，并并入 bounds；返回非空块数 */
static int diff_pieces(double ax, double ay, double aw, double ah,
                       double bx, double by, double bw, double bh,
                       double *u) {
    double ax1 = ax + aw, ay1 = ay + ah;
    double bx1 = bx + bw, by1 = by + bh;
    int pieces = 0;
    double mx0 = fmax(ax, bx), mx1 = fmin(ax1, bx1);
    struct R { double x, y, w, h; } p[4]; int np = 0;
    if (bx > ax)     p[np++] = (struct R){ax, ay, bx - ax, ah};
    if (bx1 < ax1)   p[np++] = (struct R){bx1, ay, ax1 - bx1, ah};
    if (mx1 > mx0) {
        if (by > ay)   p[np++] = (struct R){mx0, ay, mx1 - mx0, by - ay};
        if (by1 < ay1) p[np++] = (struct R){mx0, by1, mx1 - mx0, ay1 - by1};
    }
    for (int i = 0; i < np; ++i) {
        if (p[i].w <= 0 || p[i].h <= 0) continue;
        double px1 = p[i].x + p[i].w, py1 = p[i].y + p[i].h;
        if (pieces == 0) { u[0]=p[i].x; u[1]=p[i].y; u[2]=px1; u[3]=py1; }
        else {
            if (p[i].x < u[0]) u[0] = p[i].x;
            if (p[i].y < u[1]) u[1] = p[i].y;
            if (px1 > u[2]) u[2] = px1;
            if (py1 > u[3]) u[3] = py1;
        }
        ++pieces;
    }
    return pieces;
}

int cull_diff(CullView old_view, CullView new_view, CullDiff *out,
              const char **err) {
    if (!out) return CULL_E_ARG;
    if (err) *err = NULL;
    if (!valid_view(old_view, "old", err) || !valid_view(new_view, "new", err))
        return CULL_E_VIEW_EMPTY;
    out->moved = (old_view.x != new_view.x || old_view.y != new_view.y ||
                  old_view.w != new_view.w || old_view.h != new_view.h);
    double ue[4], ux[4];
    int ne = diff_pieces(new_view.x, new_view.y, new_view.w, new_view.h,
                         old_view.x, old_view.y, old_view.w, old_view.h, ue);
    int nx = diff_pieces(old_view.x, old_view.y, old_view.w, old_view.h,
                         new_view.x, new_view.y, new_view.w, new_view.h, ux);
    if (ne > 0) out->enter = (CullView){ue[0], ue[1], ue[2]-ue[0], ue[3]-ue[1]};
    else        out->enter = (CullView){new_view.x, new_view.y, 0, 0};
    if (nx > 0) out->exit_ = (CullView){ux[0], ux[1], ux[2]-ux[0], ux[3]-ux[1]};
    else        out->exit_ = (CullView){old_view.x, old_view.y, 0, 0};
    return CULL_OK;
}

static int grid_insert(CullInc *st, int id) {
    const CullBox *b = &st->boxes[id];
    int x0 = (int)((b->x - st->ox) / st->cw); if (x0 < 0) x0 = 0;
    int y0 = (int)((b->y - st->oy) / st->ch); if (y0 < 0) y0 = 0;
    int x1 = (int)((b->x + b->w - st->ox) / st->cw); if (x1 >= st->gw) x1 = st->gw - 1;
    int y1 = (int)((b->y + b->h - st->oy) / st->ch); if (y1 >= st->gh) y1 = st->gh - 1;
    for (int gy = y0; gy <= y1; ++gy)
        for (int gx = x0; gx <= x1; ++gx) {
            int c = gy * st->gw + gx;
            if (st->cell_n[c] == st->cell_cap[c]) {
                int cap = st->cell_cap[c] ? st->cell_cap[c] * 2 : 8;
                int *p = realloc(st->cells[c], sizeof(int) * (size_t)cap);
                if (!p) return 0;
                st->cells[c] = p; st->cell_cap[c] = cap;
            }
            st->cells[c][st->cell_n[c]++] = id;
        }
    return 1;
}

static int collect_region(CullInc *st, CullView r) {
    if (r.w <= 0 || r.h <= 0) return CULL_OK;
    int x0 = (int)((r.x - st->ox) / st->cw); if (x0 < 0) x0 = 0;
    int y0 = (int)((r.y - st->oy) / st->ch); if (y0 < 0) y0 = 0;
    int x1 = (int)((r.x + r.w - st->ox) / st->cw); if (x1 >= st->gw) x1 = st->gw - 1;
    int y1 = (int)((r.y + r.h - st->oy) / st->ch); if (y1 >= st->gh) y1 = st->gh - 1;
    for (int gy = y0; gy <= y1; ++gy)
        for (int gx = x0; gx <= x1; ++gx) {
            int c = gy * st->gw + gx;
            for (int k = 0; k < st->cell_n[c]; ++k) {
                int id = st->cells[c][k];
                if (st->mark[id] != (int)st->gen) {
                    st->mark[id] = (int)st->gen;
                    if (st->ncand == st->capcand) {
                        int cap = st->capcand ? st->capcand * 2 : 256;
                        int *p = realloc(st->cand, sizeof(int) * (size_t)cap);
                        if (!p) return CULL_E_NOMEM;
                        st->cand = p; st->capcand = cap;
                    }
                    st->cand[st->ncand++] = id;
                }
            }
        }
    return CULL_OK;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int cull_inc_open(const CullBox *boxes, int n, CullView view,
                  CullInc **out, const char **err) {
    if (err) *err = NULL;
    if (!boxes || !out || n < 0) return CULL_E_ARG;
    if (!valid_view(view, "view", err)) return CULL_E_VIEW_EMPTY;
    for (int i = 0; i < n; ++i) {
        if (boxes[i].w < 0) { if (err) *err = err_box(i, 'w', boxes[i].w); return CULL_E_BOX; }
        if (boxes[i].h < 0) { if (err) *err = err_box(i, 'h', boxes[i].h); return CULL_E_BOX; }
    }
    CullInc *st = calloc(1, sizeof(*st));
    if (!st) return CULL_E_NOMEM;
    st->boxes = boxes; st->n = n; st->view = view;

    double minx = 0, miny = 0, maxx = 0, maxy = 0;
    if (n > 0) {
        minx = maxx = boxes[0].x; miny = maxy = boxes[0].y;
        for (int i = 1; i < n; ++i) {
            double x1 = boxes[i].x + boxes[i].w, y1 = boxes[i].y + boxes[i].h;
            if (boxes[i].x < minx) minx = boxes[i].x;
            if (x1 > maxx) maxx = x1;
            if (boxes[i].y < miny) miny = boxes[i].y;
            if (y1 > maxy) maxy = y1;
        }
    }
    int g = (int)sqrt((double)(n > 0 ? n : 1));
    if (g < 8) g = 8;
    if (g > 200) g = 200;
    st->gw = st->gh = g;
    st->ox = minx; st->oy = miny;
    st->cw = (maxx > minx) ? (maxx - minx) / g : 1.0;
    st->ch = (maxy > miny) ? (maxy - miny) / g : 1.0;
    size_t cells = (size_t)g * g;
    st->cells = calloc(cells, sizeof(int *));
    st->cell_n = calloc(cells, sizeof(int));
    st->cell_cap = calloc(cells, sizeof(int));
    st->mark = calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    st->removed = calloc((size_t)(n > 0 ? n : 1), sizeof(char));
    if (!st->cells || !st->cell_n || !st->cell_cap || !st->mark || !st->removed)
        goto nomem;
    for (int i = 0; i < n; ++i)
        if (!grid_insert(st, i)) goto nomem;

    /* 首次可见集合：全量一次（等价 cull_run），升序 */
    st->visible = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!st->visible) goto nomem;
    for (int i = 0; i < n; ++i)
        if (overlaps(boxes[i], view)) st->visible[st->nvis++] = i;
    *out = st;
    return CULL_OK;
nomem:
    cull_inc_close(st);
    return CULL_E_NOMEM;
}

int cull_inc_snapshot(const CullInc *st, CullResult *out) {
    if (!st || !out) return CULL_E_ARG;
    out->ids = malloc(sizeof(int) * (size_t)(st->nvis > 0 ? st->nvis : 1));
    if (!out->ids) return CULL_E_NOMEM;
    memcpy(out->ids, st->visible, sizeof(int) * (size_t)st->nvis);
    out->count = st->nvis;
    return CULL_OK;
}

int cull_inc_update(CullInc *st, CullView new_view, CullIncStats *stats,
                    const char **err) {
    if (err) *err = NULL;
    if (!st) return CULL_E_STATE;
    if (!valid_view(new_view, "new_view", err)) return CULL_E_VIEW_EMPTY;
    if (stats) memset(stats, 0, sizeof(*stats));

    CullDiff d;
    int rc = cull_diff(st->view, new_view, &d, NULL);
    if (rc != CULL_OK) return rc;

    /* 无差异：集合原样返回，零候选零更新 */
    if (!d.moved) {
        if (stats) stats->candidates = stats->entered = stats->exited = stats->updated = 0;
        return CULL_OK;
    }

    st->ncand = 0; st->nadds = 0;
    ++st->gen;
    if (collect_region(st, d.enter) != CULL_OK) return CULL_E_NOMEM;
    if (collect_region(st, d.exit_) != CULL_OK) return CULL_E_NOMEM;

    for (int k = 0; k < st->ncand; ++k) {
        int id = st->cand[k];
        int now_in = overlaps(st->boxes[id], new_view);
        int was_in = vis_contains(st, id);
        if (now_in && !was_in) {
            if (st->nadds == st->capadds) {
                int cap = st->capadds ? st->capadds * 2 : 64;
                int *p = realloc(st->adds, sizeof(int) * (size_t)cap);
                if (!p) return CULL_E_NOMEM;
                st->adds = p; st->capadds = cap;
            }
            st->adds[st->nadds++] = id;
        } else if (!now_in && was_in) {
            st->removed[id] = 1;
        }
    }
    if (st->nadds > 0) qsort(st->adds, (size_t)st->nadds, sizeof(int), cmp_int);

    int removed_count = 0;
    for (int i = 0; i < st->nvis; ++i)
        if (st->removed[st->visible[i]]) ++removed_count;
    int new_count = st->nvis - removed_count + st->nadds;
    int *nv = malloc(sizeof(int) * (size_t)(new_count > 0 ? new_count : 1));
    if (!nv) return CULL_E_NOMEM;

    /* 两路归并：旧集合（跳过 removed） + 新增（已排序），结果仍升序 */
    int i = 0, j = 0, w = 0;
    while (i < st->nvis && j < st->nadds) {
        int id = st->visible[i];
        if (st->removed[id]) { ++i; continue; }
        if (id < st->adds[j]) nv[w++] = id, ++i;
        else nv[w++] = st->adds[j], ++j;
    }
    while (i < st->nvis) {
        int id = st->visible[i++];
        if (!st->removed[id]) nv[w++] = id;
    }
    while (j < st->nadds) nv[w++] = st->adds[j++];

    for (int k = 0; k < st->ncand; ++k) st->removed[st->cand[k]] = 0;
    free(st->visible);
    st->visible = nv; st->nvis = new_count;
    st->view = new_view;

    if (stats) {
        stats->candidates = st->ncand;
        stats->entered = st->nadds;
        stats->exited = removed_count;
        stats->updated = st->nadds + removed_count;
    }
    return CULL_OK;
}

void cull_inc_close(CullInc *st) {
    if (!st) return;
    if (st->cells) {
        for (int i = 0; i < st->gw * st->gh; ++i) free(st->cells[i]);
        free(st->cells);
    }
    free(st->cell_n); free(st->cell_cap);
    free(st->mark); free(st->removed);
    free(st->cand); free(st->adds); free(st->visible);
    free(st);
}
