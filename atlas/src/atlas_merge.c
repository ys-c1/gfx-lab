#include "atlas.h"
#include <stdio.h>
#include <stdlib.h>

/* 阶段一：计算各源图集的新偏移（base），并产出合并后的全局布局。
 * do_merge=0 时不合并：每个源图集独立打包，偏移按顺序槽位给出，布局照旧。 */
int atlas_merge_layout(AtlasMerge *m, int *err_at) {
    if (err_at) *err_at = -1;
    if (!m || !m->sources || m->nsrc <= 0 || m->width <= 0 || !m->positions) return -1;

    int total = 0;
    for (int s = 0; s < m->nsrc; ++s) {
        if (!m->sources[s].blocks || m->sources[s].nblocks <= 0) {
            if (err_at) *err_at = s;
            return -1;
        }
        total += m->sources[s].nblocks;
    }

    /* 合法性：块尺寸不得超过目标宽度（同时给全局块序号）。 */
    int global_idx = 0;
    for (int s = 0; s < m->nsrc; ++s) {
        for (int i = 0; i < m->sources[s].nblocks; ++i, ++global_idx) {
            AtlasRect r = m->sources[s].blocks[i];
            if (r.w <= 0 || r.h <= 0 || r.w > m->width) {
                if (err_at) *err_at = global_idx;
                return -2;
            }
        }
    }

    /* 偏移表：槽位按源图集顺序连续编号。do_merge 不改变编号规则，
     * 只改变几何布局，因此非合并模式下改写仍为恒等映射。 */
    int used_px = 0, shelves_total = 0, total_height = 0;
    for (int s = 0; s < m->nsrc; ++s) m->sources[s].base = -1;
    int base = 0;
    for (int s = 0; s < m->nsrc; ++s) {
        m->sources[s].base = base;
        base += m->sources[s].nblocks;
    }

    if (m->do_merge) {
        /* 合并：把所有源的块拼成一条序列，统一货架打包。 */
        AtlasRect *all = (AtlasRect *)malloc((size_t)total * sizeof(AtlasRect));
        if (!all) return -3;
        int k = 0;
        for (int s = 0; s < m->nsrc; ++s)
            for (int i = 0; i < m->sources[s].nblocks; ++i)
                all[k++] = m->sources[s].blocks[i];

        AtlasStats st;
        int rc = atlas_pack(all, total, m->width, m->positions, &st);
        if (rc != 0) {
            free(all);
            if (err_at) *err_at = 0;
            return -2;
        }
        used_px = st.used_px;
        shelves_total = st.shelves;
        /* 占用带高度 = 所有块底边的最大值。 */
        total_height = 0;
        for (int i = 0; i < total; ++i) {
            int bottom = m->positions[i].y + all[i].h;
            if (bottom > total_height) total_height = bottom;
        }
        free(all);
    } else {
        /* 不合并：每个源图集在各自的货架带上独立打包，位置直接落到全局数组。 */
        int cur_x = 0, cur_y = 0, shelf_h = 0;
        int off = 0;
        for (int s = 0; s < m->nsrc; ++s) {
            int n = m->sources[s].nblocks;
            cur_x = 0; shelf_h = 0;
            int local_shelves = 1;
            for (int i = 0; i < n; ++i) {
                AtlasRect r = m->sources[s].blocks[i];
                if (cur_x + r.w > m->width) {
                    cur_y += shelf_h; cur_x = 0; shelf_h = 0; ++local_shelves;
                }
                m->positions[off + i].x = cur_x;
                m->positions[off + i].y = cur_y;
                m->positions[off + i].placed = 1;
                cur_x += r.w;
                if (r.h > shelf_h) shelf_h = r.h;
                used_px += r.w * r.h;
            }
            cur_y += shelf_h;       /* 源图集之间留货架边界，互不重叠 */
            total_height = cur_y;
            shelves_total += local_shelves;
            off += n;
        }
    }

    m->stats.total_blocks = total;
    m->stats.used_px = used_px;
    m->stats.total_height = total_height;
    m->stats.shelves = shelves_total;
    m->stats.rewritten = 0;
    m->stats.errors = 0;
    return 0;
}

/* 阶段二：依据偏移表改写旧索引，并检测偏移越界与索引冲突。
 * 改写规则：new_idx = sources[src].base + old_idx。
 * 冲突检测：不同条目落到同一个全局槽位即报错（带后到条目序号与槽位号）。 */
int atlas_rewrite_indices(AtlasMerge *m, AtlasIndexEntry *entries, int nent,
                          int *err_at, int *err_slot) {
    if (err_at) *err_at = -1;
    if (err_slot) *err_slot = -1;
    if (!m || !entries || nent <= 0) return -1;

    int total = m->stats.total_blocks;
    if (total <= 0) return -1;

    int *owner = (int *)malloc((size_t)total * sizeof(int));
    if (!owner) return -3;
    for (int i = 0; i < total; ++i) owner[i] = -1;

    int rewritten = 0, errors = 0, first_rc = 0;
    for (int e = 0; e < nent; ++e) {
        AtlasIndexEntry *it = &entries[e];
        it->new_idx = -1;
        it->valid = 0;

        /* 偏移越界：源图集号非法，或局部块号超出该图集范围，或结果越过总槽位。 */
        if (it->src < 0 || it->src >= m->nsrc ||
            it->old_idx < 0 || it->old_idx >= m->sources[it->src].nblocks) {
            ++errors;
            if (first_rc == 0) { first_rc = -4; if (err_at) *err_at = e; }
            continue;
        }
        int new_idx = m->sources[it->src].base + it->old_idx;
        if (new_idx < 0 || new_idx >= total) {
            ++errors;
            if (first_rc == 0) { first_rc = -4; if (err_at) *err_at = e; }
            continue;
        }

        /* 索引冲突：同一全局槽位被两条引用占用。 */
        if (owner[new_idx] != -1) {
            ++errors;
            if (first_rc == 0) {
                first_rc = -5;
                if (err_at) *err_at = e;
                if (err_slot) *err_slot = new_idx;
            }
            continue;
        }
        owner[new_idx] = e;
        it->new_idx = new_idx;
        it->valid = 1;
        ++rewritten;
    }

    free(owner);
    m->stats.rewritten = rewritten;
    m->stats.errors = errors;
    return first_rc;
}

int atlas_export_offsets(const AtlasMerge *m, const char *path) {
    if (!m || !path) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) return -6;
    fprintf(fp, "src\tbase\tnblocks\n");
    for (int s = 0; s < m->nsrc; ++s)
        fprintf(fp, "%d\t%d\t%d\n", s, m->sources[s].base, m->sources[s].nblocks);
    fclose(fp);
    return 0;
}

int atlas_export_indices(const AtlasIndexEntry *entries, int nent, const char *path) {
    if (!entries || !path) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) return -6;
    fprintf(fp, "entry\tsrc\told_idx\tnew_idx\tvalid\n");
    for (int e = 0; e < nent; ++e)
        fprintf(fp, "%d\t%d\t%d\t%d\t%d\n", e, entries[e].src, entries[e].old_idx,
                entries[e].new_idx, entries[e].valid);
    fclose(fp);
    return 0;
}
