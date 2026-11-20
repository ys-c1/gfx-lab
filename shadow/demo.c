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
    printf("mean linear %.4f\n", sum / (128.0f * 128.0f));

    /* Linear bytes are what made the viewer look gray; srgb is the fix. */
    float *t8 = (float*)malloc(256 * sizeof(float));
    float *t16 = (float*)malloc(65536 * sizeof(float));
    if (!t8 || !t16) return 1;
    if (sh_quant_table(8, 1, SH_SRGB_GAMMA, t8) != SH_OK) return 1;
    if (sh_quant_table(16, 1, SH_SRGB_GAMMA, t16) != SH_OK) return 1;
    unsigned int c8 = 0, c16 = 0;
    sh_quantize_code(0.5f, 8, 1, SH_SRGB_GAMMA, &c8);
    sh_quantize_code(0.5f, 16, 1, SH_SRGB_GAMMA, &c16);
    printf("linear 0.5 -> srgb8 code %u (decodes to %.4f), srgb16 code %u (%.5f)\n",
           c8, t8[c8], c16, t16[c16]);
    free(t16);
    free(t8);

    int rc = sh_write_pgm(&m, "demo.pgm", 8, 1, SH_SRGB_GAMMA);
    sh_free(&m);
    return rc == 0 ? 0 : 1;
}
