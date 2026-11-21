#include "mixer.h"
#include "deess.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define SR 48000
#define PI 3.14159265358979323846
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
    int rc = mx_write_wav(&b, "demo.wav");
    mx_free(&b);
    if (rc != 0) return 1;

    /* 齿音抑制演示: 220Hz 浊音 + 两段 6.3kHz 齿音突发 */
    int frames = SR;                            /* 1s */
    float *in = (float*)calloc((size_t)frames * 2, sizeof(float));
    float *out = (float*)malloc((size_t)frames * 2 * sizeof(float));
    if (!in || !out) return 1;
    DsCfg c = ds_default_cfg(SR);
    double f0 = sqrt((double)c.band_lo_hz * c.band_hi_hz);
    for (int i = 0; i < frames; ++i) {
        float v = 0.2f * (float)sin(2.0 * PI * 220.0 * i / SR);
        if ((i >= SR/10 && i < SR/5) || (i >= 3*SR/10 && i < 9*SR/20))
            v += 0.4f * (float)sin(2.0 * PI * f0 * i / SR);
        in[i*2] = v; in[i*2+1] = v;
    }
    DsCtx *d = ds_create(&c, 2);
    if (!d) return 1;
    DsSeg segs[32]; DsStats st;
    if (ds_process(d, in, out, frames, segs, 32, &st) != 0) return 1;
    printf("deess: segments=%d max_att=%.2fdB peak_in=%.4f peak_out=%.4f\n",
           st.segments_total, st.max_att_db, st.peak_in, st.peak_out);
    for (int i = 0; i < st.segments; ++i)
        printf("  seg ch%d [%.3fs..%.3fs] att=%.2fdB ratio=%.3f\n",
               segs[i].channel, segs[i].start_frame / (double)SR,
               segs[i].end_frame / (double)SR, segs[i].att_db, segs[i].peak_ratio);
    if (ds_export_csv(d, "demo_sibilance.csv") != 0) return 1;
    ds_destroy(d);
    MixBus db;
    if (mx_init(&db, frames, 2) != 0) return 1;
    mx_add(&db, out, frames, 1.0f);
    rc = mx_write_wav(&db, "demo_deess.wav");
    mx_free(&db);
    free(in); free(out);
    printf("wrote demo_deess.wav + demo_sibilance.csv\n");
    return rc == 0 ? 0 : 1;
}
