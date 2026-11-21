#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    MixBus b;
    if (mx_init(&b, 4800, 2) != 0) return 1;
    float buf[960];
    for (int i = 0; i < 480; ++i) {
        float s = 0.3f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
        buf[i*2+0] = s; buf[i*2+1] = s;
    }
    int n = mx_add(&b, buf, 480, 1.0f);
    printf("mixed %d frames\n", n);

    /* 电平表：每块 64 帧取峰值，保持 480 帧（10 ms），每帧回落 1e-4 */
    MxMeterCfg cfg = { 64, 480, 1.0e-4f, 1 };
    float *out = (float*)malloc(sizeof(float) * (size_t)b.frames * 2);
    MxMeterReport rep;
    if (out && mx_meter_process(b.buf, b.frames, b.channels, &cfg, out, &rep) == b.frames) {
        printf("meter: peak_in=%.4f peak_out=%.4f hold=%d frames overrun=%d\n",
               (double)rep.peak_in, (double)rep.peak_out, rep.hold_observed,
               rep.overrun_blocks);
    }
    free(out);

    int rc = mx_write_wav(&b, "demo.wav");
    mx_free(&b);
    return rc == 0 ? 0 : 1;
}
