#include "mixer.h"
#include <math.h>
#include <stdio.h>

/* Three stem buses with very different levels and one shared bus at a
 * moderate setting.  Each bus is levelled against its own threshold, then
 * summed into the final mix and written as demo.wav. */
#define SR 48000
#define FRAMES 4800
#define CH 2
#define NBUS 3

int main(void) {
    static float stem[NBUS][FRAMES * CH];
    MxBusView buses[NBUS];
    MxCompCfg cfgs[NBUS];
    MxBusReport reps[NBUS];

    for (int i = 0; i < FRAMES; ++i) {
        float t = (float)i / (float)SR;
        float drums = 1.30f * sinf(2.0f * 3.14159265f * 90.0f * t) *
                      (sinf(2.0f * 3.14159265f * 3.0f * t) > 0.0f ? 1.0f : 0.25f);
        float bass = 0.55f * sinf(2.0f * 3.14159265f * 55.0f * t);
        float lead = 0.20f * sinf(2.0f * 3.14159265f * 880.0f * t);
        stem[0][i * CH + 0] = drums;
        stem[0][i * CH + 1] = drums * 0.92f;
        stem[1][i * CH + 0] = bass;
        stem[1][i * CH + 1] = bass;
        stem[2][i * CH + 0] = lead * 0.9f;
        stem[2][i * CH + 1] = lead;
    }

    for (int b = 0; b < NBUS; ++b) {
        buses[b] = (MxBusView){ stem[b], FRAMES, CH };
        cfgs[b].detector = MX_DETECT_PEAK;
        cfgs[b].threshold = 0.5f;
        cfgs[b].ratio = 2.0f + (float)b;   /* 2, 3, 4 */
        cfgs[b].enabled = 1;
    }

    int rc = mx_process_buses(buses, cfgs, NBUS, reps);
    if (rc != MX_OK) {
        printf("compression error: %s\n", mx_err_str(rc));
        return 1;
    }

    printf("%-4s %-6s %-6s %-8s %-8s %-8s %-9s %-9s %s\n",
           "bus", "thresh", "ratio", "level", "gain", "red dB",
           "peak pre", "peak post", "loud dB");
    for (int b = 0; b < NBUS; ++b) {
        const MxBusReport *r = &reps[b];
        printf("%-4d %-6.3f %-6.2f %-8.4f %-8.4f %-8.3f %-9.4f %-9.4f %+.3f\n",
               b, r->threshold, r->ratio, r->level.level, r->gain,
               r->reduction_db, r->peak_before, r->peak_after,
               r->loudness_delta_db);
    }

    rc = mx_write_bus_csv(reps, NBUS, "build/buses_demo.csv");
    if (rc != MX_OK) {
        printf("csv error: %s\n", mx_err_str(rc));
        return 1;
    }
    printf("per-bus table: build/buses_demo.csv\n");

    MixBus mix;
    if (mx_init(&mix, FRAMES, CH) != 0) return 1;
    static float sum[FRAMES * CH];
    for (int b = 0; b < NBUS; ++b)
        for (int i = 0; i < FRAMES * CH; ++i)
            sum[i] += stem[b][i];
    if (mx_add(&mix, sum, FRAMES, 1.0f) != FRAMES) {
        printf("mix failed\n");
        mx_free(&mix);
        return 1;
    }
    printf("mixed %d frames from %d compressed buses\n", mix.frames, NBUS);
    rc = mx_write_wav(&mix, "demo.wav");
    mx_free(&mix);
    return rc == MX_OK ? 0 : 1;
}
