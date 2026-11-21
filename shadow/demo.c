#include "shadow.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    ShadowMap m;
    if (sh_init(&m, 128, 128) != 0) return 1;
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};
    if (sh_render(&m, &l, segs, 2) != 0) return 1;
    float sum = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("mean %.4f\n", sum / (128.0f * 128.0f));
    int rc = sh_write_pgm(&m, "demo.pgm");
    if (rc != 0) { sh_free(&m); return 1; }

    /* 环境光染色：低速(穷举) vs 高速(色表) 核对 */
    ShTintState st;
    if (sh_tint_init(&st) != 0) { sh_free(&m); return 1; }
    ShTintConfig cfg = {1, SH_TINT_FAST, 1, {0.15f, 0.20f, 0.35f}, 0, {0, 0, 0}};
    float *rgb = (float *)malloc((size_t)128 * 128 * 3 * sizeof(float));
    if (!rgb) { sh_free(&m); return 1; }
    ShTintStats ts;
    if (sh_tint_render(&st, &m, &l, segs, 2, &cfg, rgb, &ts) != 0) return 1;
    cfg.ambient.g = 0.25f; /* 环境色变化，单独统计 */
    ShTintStats cs;
    if (sh_tint_compare(&st, &m, &l, segs, 2, &cfg, &cs) != 0) return 1;
    int tn = 0;
    const ShColor *tab = sh_tint_table(&st, &tn);
    printf("tint err %.5f exact %.3fms fast %.3fms blend %.4f px %ld\n",
           cs.tint_error, cs.time_exact_ms, cs.time_fast_ms, cs.blend_ratio, cs.pixels);
    printf("ambient_changes %ld table_rebuilds %ld table %d entries tail %.3f %.3f %.3f\n",
           cs.ambient_changes, cs.table_rebuilds, tn,
           tab ? tab[tn - 1].r : 0.0f, tab ? tab[tn - 1].g : 0.0f, tab ? tab[tn - 1].b : 0.0f);
    free(rgb);
    sh_free(&m);
    return 0;
}
