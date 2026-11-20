#include "shadow.h"
#include <stdio.h>
int main(void) {
    ShadowMap m;
    if (sh_init(&m, 128, 128) != 0) return 1;
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f, 6.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};

    /* hard shadow: identical to the original renderer */
    if (sh_render(&m, &l, segs, 2) != 0) return 1;
    float sum = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("hard mean %.4f\n", sum / (128.0f * 128.0f));
    if (sh_write_pgm(&m, "demo.pgm") != 0) return 1;

    /* soft shadow: 16 light points composited through the three stages */
    ShPenumbra pen = {1, 16};
    ShError err;
    if (sh_render_ex(&m, &l, segs, 2, &pen, &err) != 0) {
        printf("soft failed: %s (%s)\n", err.message, err.field);
        return 1;
    }
    sum = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) sum += m.buf[i];
    printf("soft mean %.4f\n", sum / (128.0f * 128.0f));
    int rc = sh_write_pgm(&m, "demo_soft.pgm");
    sh_free(&m);
    return rc == 0 ? 0 : 1;
}
