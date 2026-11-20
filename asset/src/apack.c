#include "apack.h"
#include <limits.h>
#include <stdlib.h>

static int is_pow2(unsigned int a) { return a != 0 && (a & (a - 1)) == 0; }

static unsigned long long align_up_ull(unsigned long long v, unsigned int a) {
    unsigned long long m = (unsigned long long)a - 1;
    return (v + m) & ~m;
}

/* 现状布局:按给定顺序顺次摆放,每块前补齐对齐。optimize=0 时逐字节复现。 */
static int plan_sequential(const ApBlock *blocks, int n,
                           unsigned int *offsets, unsigned int *total) {
    unsigned long long cur = 0;
    for (int i = 0; i < n; ++i) {
        cur = align_up_ull(cur, blocks[i].align);
        if (cur + blocks[i].size > UINT_MAX) return AP_PACK_ERR_OFLOW;
        offsets[i] = (unsigned int)cur;
        cur += blocks[i].size;
    }
    *total = (unsigned int)cur;
    return AP_PACK_OK;
}

typedef struct { unsigned int align, size; int index; } Item;
typedef struct { unsigned int start, end; } Gap; /* [start, end) */

/* 对齐降序、同对齐按大小降序、再按下标升序,保证确定性。 */
static int cmp_item(const void *pa, const void *pb) {
    const Item *a = (const Item *)pa, *b = (const Item *)pb;
    if (a->align != b->align) return a->align > b->align ? -1 : 1;
    if (a->size != b->size) return a->size > b->size ? -1 : 1;
    return a->index < b->index ? -1 : 1;
}

/* 紧凑布局:按(对齐,大小)降序摆放;每块先在已产生的对齐空隙里做
 * best-fit 回收,放不下才追加到末尾并产生新空隙。
 * 结果与顺序布局取较优者,保证不劣于现状。 */
static int plan_tight(const ApBlock *blocks, int n,
                      unsigned int *offsets, unsigned int *total) {
    unsigned long long seq = 0; /* 顺序布局跨度,作为对照上界 */
    for (int i = 0; i < n; ++i) {
        seq = align_up_ull(seq, blocks[i].align);
        if (seq + blocks[i].size > UINT_MAX) return AP_PACK_ERR_OFLOW;
        seq += blocks[i].size;
    }
    Item *items = (Item *)malloc((size_t)n * sizeof *items);
    Gap *gaps = (Gap *)malloc((size_t)(2 * n + 1) * sizeof *gaps);
    if (!items || !gaps) { free(items); free(gaps); return AP_PACK_ERR_NOMEM; }
    for (int i = 0; i < n; ++i) {
        items[i].align = blocks[i].align;
        items[i].size = blocks[i].size;
        items[i].index = i;
    }
    qsort(items, (size_t)n, sizeof *items, cmp_item);
    int ngaps = 0;
    unsigned long long cur = 0;
    for (int k = 0; k < n; ++k) {
        unsigned int a = items[k].align, s = items[k].size;
        int best = -1;
        unsigned long long best_pos = 0, best_left = ULLONG_MAX;
        for (int g = 0; g < ngaps; ++g) {
            unsigned long long pos = align_up_ull(gaps[g].start, a);
            if (pos + s <= gaps[g].end) {
                unsigned long long left = (unsigned long long)gaps[g].end - (pos + s);
                if (left < best_left) { best_left = left; best = g; best_pos = pos; }
            }
        }
        unsigned long long pos;
        if (best >= 0) {
            unsigned int gs = gaps[best].start, ge = gaps[best].end;
            unsigned int right = (unsigned int)(best_pos + s);
            pos = best_pos;
            if (right < ge) gaps[best].start = right;      /* 右段继续留用 */
            else gaps[best] = gaps[--ngaps];               /* 用尽,移除 */
            if ((unsigned long long)gs < pos) {            /* 左段对齐余量也回收 */
                gaps[ngaps].start = gs;
                gaps[ngaps].end = (unsigned int)pos;
                ++ngaps;
            }
        } else {
            pos = align_up_ull(cur, a);
            if (pos + s > UINT_MAX) { free(items); free(gaps); return AP_PACK_ERR_OFLOW; }
            if (pos > cur) {
                gaps[ngaps].start = (unsigned int)cur;
                gaps[ngaps].end = (unsigned int)pos;
                ++ngaps;
            }
            cur = pos + s;
        }
        offsets[items[k].index] = (unsigned int)pos;
    }
    if (cur > seq) { /* 极少数实例排序反而更差,回退为顺序布局 */
        free(items);
        free(gaps);
        return plan_sequential(blocks, n, offsets, total);
    }
    *total = (unsigned int)cur;
    free(items);
    free(gaps);
    return AP_PACK_OK;
}

int ap_pack_plan(const ApBlock *blocks, int n, int optimize,
                 unsigned int *offsets, unsigned int *total,
                 ApPackError *err) {
    if (err) { err->index = -1; err->field = 0; }
    if (n < 0 || !total || (n > 0 && (!blocks || !offsets))) return AP_PACK_ERR_ARG;
    for (int i = 0; i < n; ++i) {
        if (!is_pow2(blocks[i].align)) {
            if (err) { err->index = i; err->field = "align"; }
            return AP_PACK_ERR_ALIGN;
        }
        if (blocks[i].size == 0) {
            if (err) { err->index = i; err->field = "size"; }
            return AP_PACK_ERR_SIZE;
        }
    }
    if (n == 0) { *total = 0; return AP_PACK_OK; }
    if (!optimize) return plan_sequential(blocks, n, offsets, total);
    return plan_tight(blocks, n, offsets, total);
}

void ap_pack_apply(ApEntry *entries, const unsigned int *offsets, int n) {
    if (!entries || !offsets || n <= 0) return;
    for (int i = 0; i < n; ++i) entries[i].offset = offsets[i];
}
