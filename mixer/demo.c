#include "mixer.h"
#include <math.h>
#include <stdio.h>
int main(void) {
    MixBus b;
    if (mx_init(&b, 4800, 2) != 0) return 1;
    float buf[960], sum[960];
    for (int i = 0; i < 960; ++i) sum[i] = 0.0f;
    /* 8 simultaneous tracks summed on the same frames: unprotected peak ~2x */
    for (int t = 0; t < 8; ++t) {
        for (int i = 0; i < 480; ++i) {
            float s = 0.35f * sinf(2.0f * 3.14159265f * (440.0f + 55.0f * t) *
                                   (float)i / 48000.0f);
            buf[i*2+0] = s; buf[i*2+1] = s;
        }
        for (int i = 0; i < 960; ++i) sum[i] += buf[i];
    }
    mx_add(&b, sum, 480, 1.0f);
    printf("mixed %d frames from 8 tracks\n", b.frames);

    MxLimitCfg cfg = { 1.0f, 48000, 1.0f, 20.0f, 1 };
    MxLimitReport rep = {0};
    int rc = mx_limit(&b, &cfg, &rep);
    if (rc != MX_OK) { printf("limiter error: %s\n", mx_err_str(rc)); return 1; }
    printf("peak before=%.4f after=%.4f plateau_runs=%d plateau_samples=%d\n",
           rep.peak_before, rep.peak_after, rep.plateau_runs, rep.plateau_samples);

    rc = mx_write_wav(&b, "demo.wav");
    mx_free(&b);
    return rc == 0 ? 0 : 1;
}
