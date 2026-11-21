#include "reindex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void seterr(RxErr *err, int code, int item, const char *param) {
    if (!err) return;
    err->code = code;
    err->item = item;
    err->param = param;
}

const char *rx_strerror(int code) {
    switch (code) {
    case RX_OK:         return "ok";
    case RX_EINVAL:     return "invalid argument";
    case RX_ERANGE:      return "index out of range";
    case RX_ECONFLICT:   return "mapping conflict";
    case RX_EIO:         return "io error";
    default:             return "unknown error";
    }
}

void rx_free(RxTable *table) {
    if (!table) return;
    free(table->old_to_new);
    table->old_to_new = NULL;
    table->n = 0;
    table->conflicts = 0;
}

int rx_build(const int *perm, int n, RxTable *out, RxErr *err) {
    if (!perm || !out) { seterr(err, RX_EINVAL, -1, "perm/out"); return RX_EINVAL; }
    if (n < 0) { seterr(err, RX_EINVAL, -1, "n"); return RX_EINVAL; }
    out->n = 0;
    out->old_to_new = NULL;
    out->conflicts = 0;
    if (n == 0) return RX_OK;

    int *map = (int *)malloc(sizeof(int) * (size_t)n);
    if (!map) { seterr(err, RX_EINVAL, -1, "alloc"); return RX_EINVAL; }
    for (int i = 0; i < n; ++i) map[i] = -1;

    /* 记录每个新槽位已被哪个旧槽位认领，用于查冲突。 */
    int *owner = (int *)malloc(sizeof(int) * (size_t)n);
    if (!owner) { free(map); seterr(err, RX_EINVAL, -1, "alloc"); return RX_EINVAL; }
    for (int i = 0; i < n; ++i) owner[i] = -1;

    int conflicts = 0;
    for (int i = 0; i < n; ++i) {
        if (perm[i] < 0) {
            free(map); free(owner);
            seterr(err, RX_EINVAL, i, "perm");
            return RX_EINVAL;
        }
        if (perm[i] >= n) {
            free(map); free(owner);
            seterr(err, RX_ERANGE, i, "perm");
            return RX_ERANGE;
        }
        if (owner[perm[i]] >= 0) {
            /* 同一新槽位被重复认领：先认领者生效，后者记冲突、不建映射。 */
            ++conflicts;
            continue;
        }
        owner[perm[i]] = i;
        map[i] = perm[i];
    }
    free(owner);
    out->n = n;
    out->old_to_new = map;
    out->conflicts = conflicts;
    return RX_OK;
}

int rx_rewrite(const RxTable *table, int *refs, int m,
               int *conflicts, RxErr *err) {
    if (!table || !refs) { seterr(err, RX_EINVAL, -1, "table/refs"); return RX_EINVAL; }
    if (m < 0) { seterr(err, RX_EINVAL, -1, "m"); return RX_EINVAL; }
    if (conflicts) *conflicts = 0;
    int rewritten = 0, clash = 0;
    for (int i = 0; i < m; ++i) {
        if (refs[i] < 0) {
            if (conflicts) *conflicts = clash;
            seterr(err, RX_EINVAL, i, "refs");
            return RX_EINVAL;
        }
        if (refs[i] >= table->n || table->old_to_new[refs[i]] < 0) {
            /* 越界引用或命中建表冲突槽位：保留原值并计冲突。 */
            ++clash;
            continue;
        }
        refs[i] = table->old_to_new[refs[i]];
        ++rewritten;
    }
    if (conflicts) *conflicts = clash;
    return rewritten;
}

int rx_apply(const int *perm, int n, int *refs, int m, int enabled,
             RxTable *table, int *rewritten, int *conflicts, RxErr *err) {
    if (enabled < 0) { seterr(err, RX_EINVAL, -1, "enabled"); return RX_EINVAL; }
    if (n < 0) { seterr(err, RX_EINVAL, -1, "n"); return RX_EINVAL; }
    if (m < 0) { seterr(err, RX_EINVAL, -1, "m"); return RX_EINVAL; }
    if (!perm || !refs) { seterr(err, RX_EINVAL, -1, "perm/refs"); return RX_EINVAL; }
    if (rewritten) *rewritten = 0;
    if (conflicts) *conflicts = 0;

    /* 关闭路径：什么都不做，调用方输出与旧路径完全一致。 */
    if (!enabled) return RX_OK;

    RxTable local, *t = table;
    if (!t) {
        t = &local;
        t->n = 0; t->old_to_new = NULL; t->conflicts = 0;
    }
    int rc = rx_build(perm, n, t, err);
    if (rc != RX_OK) {
        if (t == &local) rx_free(&local);
        return rc;
    }
    int clash = 0;
    int rw = rx_rewrite(t, refs, m, &clash, err);
    if (rw < 0) {
        if (t == &local) rx_free(&local);
        return rw;
    }
    if (rewritten) *rewritten = rw;
    if (conflicts) *conflicts = clash;
    if (t == &local) rx_free(&local);
    return RX_OK;
}

int rx_export(const RxTable *table, const char *path) {
    if (!table || !path || !table->old_to_new) return RX_EINVAL;
    FILE *f = fopen(path, "w");
    if (!f) return RX_EIO;
    fprintf(f, "# reindex mapping: n=%d conflicts=%d length=%d\n",
            table->n, table->conflicts, table->n);
    fprintf(f, "old,new\n");
    for (int i = 0; i < table->n; ++i)
        if (table->old_to_new[i] >= 0)
            fprintf(f, "%d,%d\n", i, table->old_to_new[i]);
    if (fclose(f) != 0) return RX_EIO;
    return RX_OK;
}
