#include "shadow.h"
#include <stdio.h>

int main(void) {
    ShadowMap m;
    if (sh_init(&m, 128, 128) != 0) return 1;

    ShLight lights[2] = {
        { 64.0f, 10.0f, 40.0f, 1.0f },
        {  4.0f, 96.0f, 30.0f, 0.6f },
    };
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};

    ShStats st;
    ShError err;
    if (sh_render_lights(&m, lights, 2, segs, 2, 1, &err, &st) != SH_OK) {
        fprintf(stderr, "shadow: %s\n", err.msg);
        sh_free(&m);
        return 1;
    }

    float sum = 0.0f, peak = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) {
        sum += m.buf[i];
        if (m.buf[i] > peak) peak = m.buf[i];
    }
    printf("lights %d, passes %d, temp pixels %zu, composite ops %zu\n",
           (int)2, st.passes, st.temp_pixels, st.blend_ops);
    printf("mean %.4f, peak %.4f\n", sum / (128.0f * 128.0f), peak);

    int rc = sh_write_pgm(&m, "demo.pgm");
    sh_free(&m);
    return rc == 0 ? 0 : 1;
}
