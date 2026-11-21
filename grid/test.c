#include "grid.h"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

int main(void) {
    /* 4 boxes: 0 and 1 touch across a cell border and overlap each other,
     * 2 chains onto 1 (transitive), 3 is isolated. */
    GBox b[4] = {
        { 0, 0, 10, 10, 0},   /* 0 */
        { 9, 0, 10, 10, 0},   /* 1 overlaps 0 */
        {18, 0, 10, 10, 0},   /* 2 overlaps 1, not 0 */
        {50,50,  5,  5, 1},   /* 3 isolated */
    };

    /* 5x5 cells of width 10 -> group {0,1,2} spans 3 cells before merge */
    Grid g;
    CK(grid_build(b, 4, 0, 0, 50, 50, 5, 5, &g) == 0);
    int dup_before = grid_duplicates(&g);
    CK(dup_before > 0);                 /* the reported bug */
    CK(g.candidates >= 6);              /* 0,1,2 each land in >=2 cells... */
    long long cand_before = g.candidates;
    grid_free(&g);

    GridStats st;
    grid_stats_init(&st);
    CK(grid_merge_groups(b, 4, &st) == 0);
    CK(st.group_count == 2);
    CK(st.merge_pairs == 2);            /* (0,1) and (1,2) */
    CK(st.group_ids[0] == st.group_ids[1]);
    CK(st.group_ids[1] == st.group_ids[2]);
    CK(st.group_ids[3] != st.group_ids[0]);
    /* merged AABB of 0..2 covers x in [0,28] */
    GBox m = st.group_boxes[st.group_ids[0]];
    CK(m.x == 0.0 && m.y == 0.0 && m.w == 28.0 && m.h == 10.0);

    /* second pass: rebuild from merged boxes with unique owner-cell
     * assignment, so each group is scheduled exactly once. */
    Grid g2;
    CK(grid_assign_unique(st.group_boxes, st.group_count,
                          0, 0, 50, 50, 5, 5, &g2) == 0);
    int dup_after = grid_duplicates(&g2);
    CK(dup_after == 0);
    CK(g2.candidates <= cand_before);  /* no more memberships than before */
    /* every merged group id appears exactly once across all cells */
    for (int k = 1; k < g2.n; ++k)
        for (int j = 0; j < k; ++j)
            CK(g2.ids[k] != g2.ids[j]);
    CK(g2.n == st.group_count);
    grid_free(&g2);
    grid_stats_free(&st);

    /* limit errors must surface the cap value */
    CK(grid_build(b, 4, 0, 0, 50, 50, 2000, 2000, &g) == -3);
    printf("limit on cell overflow: %s\n", grid_last_limit());
    CK(grid_build(b, GRID_MAX_OBJECTS + 1, 0, 0, 50, 50, 5, 5, &g) == -2);
    printf("limit on object overflow: %s\n", grid_last_limit());

    /* degenerate geometry / bad box */
    CK(grid_build(b, 4, 0, 0, 0, 50, 5, 5, &g) == -4);
    GBox bad[1] = {{0, 0, 0, 5, 0}};
    CK(grid_build(bad, 1, 0, 0, 50, 50, 5, 5, &g) == -5);
    CK(grid_build(NULL, 4, 0, 0, 50, 50, 5, 5, &g) == -1);

    /* empty input is legal */
    CK(grid_build(b, 0, 0, 0, 50, 50, 5, 5, &g) == 0);
    CK(grid_duplicates(&g) == 0 && g.candidates == 0);
    grid_free(&g);
    GridStats est; grid_stats_init(&est);
    CK(grid_merge_groups(b, 0, &est) == 0 && est.group_count == 0);
    grid_stats_free(&est);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
