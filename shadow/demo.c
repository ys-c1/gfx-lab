#include "shadow.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    ShadowMap m;
    if (sh_init(&m, 128, 128) != 0) return 1;
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};

    /* Two layers: build the light frustum, cull once, render candidates. */
    ShCullOpts opts;
    memset(&opts, 0, sizeof opts);
    opts.enabled = 1;
    ShError err;
    if (sh_frustum_from_light(&m, &l, &opts.frustum, &err) != SH_OK) return 1;
    ShCullStats st;
    if (sh_render_ex(&m, &l, segs, 2, &opts, &st, &err) != SH_OK) return 1;

    float sum = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("mean %.4f\n", sum / (128.0f * 128.0f));
    printf("segments %d culled %d candidates %d\n", st.input, st.culled, st.candidates);
    int rc = sh_write_pgm(&m, "demo.pgm");
    sh_free(&m);
    return rc == 0 ? 0 : 1;
}
