#include "atlas.h"
#include <stdlib.h>

/* ---------- 旧路径：一次性货架打包（输出保持不变） ---------- */

int atlas_pack_ex(const AtlasRect *rects, int n, int width, AtlasPos *out,
                  AtlasStats *stats, int *err_item) {
    if (err_item) *err_item = -1;
    if (!rects || !out || !stats || n <= 0 || width <= 0) return ATLAS_EARG;
    int cur_x = 0, cur_y = 0, shelf_h = 0, used = 0, shelves = 1;
    for (int i = 0; i < n; ++i) {
        if (rects[i].w <= 0 || rects[i].h <= 0 || rects[i].w > width) {
            if (err_item) *err_item = i; /* 指出是哪一项 */
            return ATLAS_ERECT;
        }
        if (cur_x + rects[i].w > width) {
            cur_y += shelf_h; cur_x = 0; shelf_h = 0; ++shelves;
        }
        out[i].x = cur_x; out[i].y = cur_y; out[i].placed = 1;
        cur_x += rects[i].w;
        if (rects[i].h > shelf_h) shelf_h = rects[i].h;
        used += rects[i].w * rects[i].h;
    }
    stats->width = width;
    stats->used_px = used;
    stats->shelves = shelves;
    return ATLAS_OK;
}

int atlas_pack(const AtlasRect *rects, int n, int width, AtlasPos *out, AtlasStats *stats) {
    return atlas_pack_ex(rects, n, width, out, stats, NULL);
}

/* ---------- 新路径：固定容量图集 + 两段式 LRU 回收 ---------- */

/* 货架游标放置；成功返回 1 并推进游标，高度放不下返回 0（不修改状态）。 */
static int cursor_place(Atlas *a, AtlasRect r, AtlasPos *p) {
    int cx = a->cur_x, cy = a->cur_y, sh = a->shelf_h, wrapped = 0;
    if (cx + r.w > a->width) { cy += sh; cx = 0; sh = 0; wrapped = 1; }
    if (cy + r.h > a->height) return 0;
    p->x = cx; p->y = cy; p->placed = 1;
    a->cur_x = cx + r.w;
    a->cur_y = cy;
    a->shelf_h = r.h > sh ? r.h : sh;
    a->shelves += wrapped;
    return 1;
}

/* 模拟：存活块（槽位序）+ 候选块能否在 width*height 内摆下。 */
static int would_fit(const Atlas *a, AtlasRect extra) {
    int cx = 0, cy = 0, sh = 0;
    for (int s = 0; s < a->n; ++s) {
        AtlasRect t;
        if (!a->blocks[s].live) continue;
        t = a->blocks[s].rect;
        if (cx + t.w > a->width) { cy += sh; cx = 0; sh = 0; }
        if (cy + t.h > a->height) return 0;
        cx += t.w;
        if (t.h > sh) sh = t.h;
    }
    if (cx + extra.w > a->width) { cy += sh; cx = 0; sh = 0; }
    if (cy + extra.h > a->height) return 0;
    return 1;
}

/* 重排索引：存活块压缩到槽位 0..n-1（保持槽位序），并按货架算法重摆。 */
static void compact(Atlas *a) {
    AtlasBlock *tmp = malloc((size_t)(a->n ? a->n : 1) * sizeof *tmp);
    int m = 0;
    if (!tmp) return; /* 分配失败则保持原状（位置仍有效，仅索引未压缩） */
    for (int s = 0; s < a->n; ++s)
        if (a->blocks[s].live) tmp[m++] = a->blocks[s];
    a->cur_x = a->cur_y = a->shelf_h = 0;
    a->shelves = 1;
    a->used_px = 0;
    for (int i = 0; i < m; ++i) {
        cursor_place(a, tmp[i].rect, &tmp[i].pos); /* would_fit 已保证放得下 */
        a->used_px += tmp[i].rect.w * tmp[i].rect.h;
    }
    for (int i = 0; i < m; ++i) a->blocks[i] = tmp[i];
    a->n = m;
    free(tmp);
}

static int log_push(Atlas *a, int id) {
    if (a->recycle_log_n == a->recycle_log_cap) {
        long nc = a->recycle_log_cap ? a->recycle_log_cap * 2 : 64;
        int *nl = realloc(a->recycle_log, (size_t)nc * sizeof *nl);
        if (!nl) return -1;
        a->recycle_log = nl;
        a->recycle_log_cap = nc;
    }
    a->recycle_log[a->recycle_log_n++] = id;
    return 0;
}

/* 两段式回收：先按最近使用时间标记，再回收最久未用并重排索引。 */
static int recycle_for(Atlas *a, AtlasRect r) {
    int m = a->n, k = 0;
    int *mark = malloc((size_t)(m ? m : 1) * sizeof *mark);
    if (!mark) return ATLAS_EARG;

    /* 第一段：标记 —— 按 last_used 升序排列存活槽位；
     * 并列时按索引号 id 升序，保证回收顺序可复现（插入排序，稳定）。 */
    for (int i = 0; i < m; ++i) mark[i] = i;
    for (int i = 1; i < m; ++i) {
        int s = mark[i], j = i - 1;
        unsigned long lu = a->blocks[s].last_used;
        int id = a->blocks[s].id;
        while (j >= 0 && (a->blocks[mark[j]].last_used > lu ||
               (a->blocks[mark[j]].last_used == lu && a->blocks[mark[j]].id > id))) {
            mark[j + 1] = mark[j];
            --j;
        }
        mark[j + 1] = s;
    }

    /* 第二段：回收 —— 从最久未用起逐块回收，直到新块放得下且有空槽。 */
    while ((m - k >= a->cap || !would_fit(a, r)) && k < m) {
        int s = mark[k++];
        a->blocks[s].live = 0;
        if (log_push(a, a->blocks[s].id) != 0) { free(mark); return ATLAS_EARG; }
    }
    free(mark);
    if (m - k >= a->cap || !would_fit(a, r)) return ATLAS_EFULL;
    compact(a); /* 重排索引 */
    return ATLAS_OK;
}

int atlas_open(Atlas *a, int width, int height, int cap, int recycling) {
    if (!a || width <= 0 || height <= 0 || cap <= 0) return ATLAS_EARG;
    if (recycling != 0 && recycling != 1) return ATLAS_EARG;
    a->blocks = calloc((size_t)cap, sizeof *a->blocks);
    if (!a->blocks) return ATLAS_EARG;
    a->width = width;
    a->height = height;
    a->cap = cap;
    a->recycling = recycling;
    a->n = 0;
    a->shelves = 1;
    a->used_px = 0;
    a->tick = 0;
    a->next_id = 0;
    a->cur_x = a->cur_y = a->shelf_h = 0;
    a->recycle_log = NULL;
    a->recycle_log_n = 0;
    a->recycle_log_cap = 0;
    return ATLAS_OK;
}

void atlas_close(Atlas *a) {
    if (!a) return;
    free(a->blocks);
    free(a->recycle_log);
    a->blocks = NULL;
    a->recycle_log = NULL;
    a->n = 0;
}

int atlas_add(Atlas *a, AtlasRect r, int *err_item) {
    AtlasPos p;
    int rc;
    if (err_item) *err_item = -1;
    if (!a || !a->blocks) return ATLAS_EARG;
    if (r.w <= 0 || r.h <= 0 || r.w > a->width || r.h > a->height) {
        if (err_item) *err_item = 0;
        return ATLAS_ERECT;
    }
    if (a->n >= a->cap) {
        if (!a->recycling) return ATLAS_EFULL;
        rc = recycle_for(a, r);
        if (rc != ATLAS_OK) return rc;
    }
    if (!cursor_place(a, r, &p)) {
        if (!a->recycling) return ATLAS_EFULL; /* 满则报错，不再崩溃 */
        rc = recycle_for(a, r);
        if (rc != ATLAS_OK) return rc;
        if (!cursor_place(a, r, &p)) return ATLAS_EFULL;
    }
    AtlasBlock *b = &a->blocks[a->n++];
    b->rect = r;
    b->pos = p;
    b->live = 1;
    b->last_used = ++a->tick;
    b->id = a->next_id++;
    a->used_px += r.w * r.h;
    return a->n - 1;
}

int atlas_add_many(Atlas *a, const AtlasRect *rects, int n, int *err_item) {
    if (err_item) *err_item = -1;
    if (!rects || n <= 0) return ATLAS_EARG;
    for (int i = 0; i < n; ++i) {
        int rc = atlas_add(a, rects[i], NULL);
        if (rc < 0) { /* atlas_add 成功时返回槽位号(>=0)，负值才是错误 */
            if (err_item) *err_item = i; /* 指出是哪一项 */
            return rc;
        }
    }
    return ATLAS_OK;
}

int atlas_touch(Atlas *a, int slot) {
    if (!a || !a->blocks || slot < 0 || slot >= a->n || !a->blocks[slot].live)
        return ATLAS_EARG;
    a->blocks[slot].last_used = ++a->tick;
    return ATLAS_OK;
}

double atlas_occupancy(const Atlas *a) {
    if (!a || a->width <= 0 || a->height <= 0) return 0.0;
    return (double)a->used_px / ((double)a->width * (double)a->height);
}

const int *atlas_recycled(const Atlas *a, long *count) {
    if (count) *count = a ? a->recycle_log_n : 0;
    return a ? a->recycle_log : NULL;
}
