#include "shadow.h"
#include <stdio.h>
int main(void) {
    ShadowMap m;
    if (sh_init(&m, 128, 128) != 0) return 1;
    ShLight l = {64.0f, 10.0f, 40.0f, 1.0f};
    ShSeg segs[2] = {{20.0f, 60.0f, 60.0f, 60.0f}, {70.0f, 70.0f, 110.0f, 70.0f}};

    if (sh_render(&m, &l, segs, 2) != 0) return 1;
    float hard = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) hard += m.buf[i];
    if (sh_write_pgm(&m, "demo_hard.pgm") != 0) return 1;

    /* 4x4 stratified probes in a 5-pixel kernel: penumbra width 4 px */
    ShPCF pcf = {4, 5};
    if (sh_render_pcf(&m, &l, segs, 2, &pcf) != 0) return 1;
    float soft = 0.0f;
    for (int i = 0; i < 128 * 128; ++i) soft += m.buf[i];
    if (sh_write_pgm(&m, "demo_soft.pgm") != 0) return 1;

    ShKernel k;
    if (sh_kernel_build(&k, 4, 5) == 0) {
        sh_kernel_write(&k, "demo_kernel.txt");
        sh_kernel_free(&k);
    }

    printf("hard mean %.6f  soft mean %.6f\n",
           hard / (128.0f * 128.0f), soft / (128.0f * 128.0f));
    sh_free(&m);
    return 0;
}
