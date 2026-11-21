#include "reverb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

#define RATE   48000
#define DECAY  0.5f
#define NTAPS  3

static const int   DELAY[NTAPS] = {97, 131, 181};
static const float GAIN[NTAPS]  = {0.9f, 0.7f, 0.5f};

static int make(Reverb *r, int ch) {
    return rv_init(r, ch, RATE, NTAPS, DELAY, GAIN, DECAY);
}

/* Stage 1: tail length is ceil(decay_sec * rate). */
static void test_tail_len(void) {
    int n = -1;
    CK(rv_tail_len(RATE, 0.5f, &n) == RV_OK && n == 24000);
    CK(rv_tail_len(RATE, 0.1f, &n) == RV_OK && n == 4800);
    CK(rv_tail_len(44100, 1.0f, &n) == RV_OK && n == 44100);
    CK(rv_tail_len(1000, 0.0001f, &n) == RV_OK && n == 1);       /* never 0 */
    CK(rv_tail_len(0, 0.5f, &n) == RV_ERR_RATE);
    CK(rv_tail_len(RATE, 0.0f, &n) == RV_ERR_DECAY);
    CK(rv_tail_len(RATE, -1.0f, &n) == RV_ERR_DECAY);
    CK(rv_tail_len(RATE, INFINITY, &n) == RV_ERR_DECAY);
    CK(rv_tail_len(RATE, NAN, &n) == RV_ERR_DECAY);
    CK(rv_tail_len(RATE, 0.5f, NULL) == RV_ERR_ARG);
    CK(strstr(rv_strerror(RV_ERR_EMPTY), "empty") != NULL);

    /* An error leaves the caller's value alone. */
    n = 1234;
    CK(rv_tail_len(RATE, 0.0f, &n) == RV_ERR_DECAY && n == 1234);
}

/* Stage 2: the padded buffer is the input followed by exactly `tail` zero
 * frames, and tail == 0 is an exact copy. */
static void test_pad(void) {
    float in[6 * 2] = {1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6};
    float out[10 * 2];
    for (int i = 0; i < 20; ++i) out[i] = 99.0f;

    CK(rv_pad(in, 6, 2, 4, out) == 10);
    CK(memcmp(out, in, sizeof(in)) == 0);
    int zeros_ok = 1, guard_ok = 1;
    for (int i = 12; i < 20; ++i) if (out[i] != 0.0f) zeros_ok = 0;
    for (int i = 0; i < 20; ++i) if (out[i] == 99.0f) guard_ok = 0;
    CK(zeros_ok);          /* exactly 4 frames * 2 ch of zeros, no more */
    CK(guard_ok);

    CK(rv_pad(in, 6, 2, 0, out) == 6);
    CK(memcmp(out, in, sizeof(in)) == 0);   /* old behaviour: plain copy */

    CK(rv_pad(in, 0, 2, 4, out) == RV_ERR_EMPTY);
    CK(rv_pad(NULL, 6, 2, 4, out) == RV_ERR_ARG);
    CK(rv_pad(in, 6, 0, 4, out) == RV_ERR_CHANNELS);
    CK(rv_pad(in, 6, 2, -1, out) == RV_ERR_TAIL);
    CK(strstr(rv_strerror(RV_ERR_EMPTY), "frames") != NULL);
    CK(strstr(rv_strerror(RV_ERR_TAIL), "tail") != NULL);
}

/* Stage 3 against the closed form: h[(k+1) d] = wet * gain * fb^k. */
static void test_sum_hand(void) {
    Reverb r;
    CK(make(&r, 2) == RV_OK);
    rv_set_mix(&r, 0.0f, 1.0f);                 /* wet only */

    enum { N = 2048 };
    float in[N * 2] = {0}, out[N * 2];
    in[0] = in[1] = 1.0f;
    CK(rv_sum(&r, in, N, out) == RV_OK);

    double maxerr = 0.0;
    for (int n = 0; n < N; ++n) {
        double hand = 0.0;
        if (n > 0)                              /* no arrival before the first delay */
            for (int t = 0; t < NTAPS; ++t) {
                if (n % DELAY[t] != 0) continue;
                hand += (double)GAIN[t] * pow((double)r.fb[t], (double)(n / DELAY[t] - 1));
            }
        for (int c = 0; c < 2; ++c) {
            double e = fabs((double)out[n * 2 + c] - hand);
            if (e > maxerr) maxerr = e;
        }
    }
    CK(maxerr < 1e-5);   /* float comb accumulation: ~6e-8 absolute */

    /* RT60: one UI at decay_sec seconds has lost 60 dB (within one comb
     * period, since only whole delays are observable). */
    float fb = rv_tap_feedback(DELAY[0], RATE, DECAY);
    CK(fabs((double)fb - pow(10.0, -3.0 * DELAY[0] / ((double)DECAY * RATE))) < 1e-7);
    int n60 = (int)lround((double)DECAY * RATE);
    for (int t = 0; t < NTAPS; ++t) {
        int k = n60 / DELAY[t] - 1;                 /* last arrival before n60 */
        double db = 20.0 * (double)k * log10((double)r.fb[t]);
        CK(fabs(db + 60.0) < 1.0);
    }

    rv_free(&r);
}

/* Old behaviour (tail dropped) is bit-identical to tail = 0, and the full
 * result starts with it because the network is causal. */
static void test_legacy_equivalence(void) {
    Reverb a, b;
    CK(make(&a, 2) == RV_OK);
    CK(make(&b, 2) == RV_OK);
    rv_set_mix(&a, 1.0f, 0.35f);
    rv_set_mix(&b, 1.0f, 0.35f);

    enum { N = 512, TAIL = 1000 };
    /* rv_process derives the tail from RATE/DECAY = 24000 frames; give it
     * that much room so the full three-stage result fits. */
    static float in[N * 2], old[N * 2];
    static float full[(N + 24000) * 2];
    for (int i = 0; i < N * 2; ++i) in[i] = 0.01f * (float)((i * 37) % 23) - 0.1f;

    CK(rv_process_legacy(&a, in, N, old) == N);
    int t = -1;
    CK(rv_process(&b, in, N, full, &t) == N + 24000);
    CK(t == 24000);
    CK(memcmp(old, full, sizeof(old)) == 0);       /* prefix is unchanged */

    /* And the explicit TAIL = 1000 path agrees with the derived one too. */
    Reverb c;
    CK(make(&c, 2) == RV_OK);
    rv_set_mix(&c, 1.0f, 0.35f);
    static float short_out[(N + TAIL) * 2];
    CK(rv_process_tail(&c, in, N, TAIL, short_out) == N + TAIL);
    CK(memcmp(short_out, full, sizeof(short_out)) == 0);   /* causal prefix */
    rv_free(&c);

    rv_free(&a);
    rv_free(&b);
}

/* Two independent paths to "tail = 0": rv_process_tail and the legacy call. */
static void test_process_tail_zero(void) {
    Reverb a, b;
    CK(make(&a, 1) == RV_OK);
    CK(make(&b, 1) == RV_OK);
    rv_set_mix(&a, 1.0f, 0.5f);
    rv_set_mix(&b, 1.0f, 0.5f);

    enum { N = 300 };
    float in[N], o1[N], o2[N];
    for (int i = 0; i < N; ++i) in[i] = sinf(0.01f * (float)i);

    CK(rv_process_tail(&a, in, N, 0, o1) == N);
    CK(rv_process_legacy(&b, in, N, o2) == N);
    CK(memcmp(o1, o2, sizeof(o1)) == 0);
    rv_free(&a);
    rv_free(&b);
}

/* Dry-only output is an exact copy, but the network still advances. */
static void test_dry_passthrough(void) {
    Reverb r;
    CK(make(&r, 2) == RV_OK);
    rv_set_mix(&r, 1.0f, 0.0f);
    enum { N = 64 };
    float in[N * 2], out[(N + 8) * 2];
    for (int i = 0; i < N * 2; ++i) in[i] = 0.001f * (float)((i * 13) % 17);
    CK(rv_process_tail(&r, in, N, 8, out) == N + 8);
    CK(memcmp(out, in, sizeof(in)) == 0);                    /* dry, bit exact */
    for (int i = N * 2; i < (N + 8) * 2; ++i) CK(out[i] == 0.0f); /* tail silent */
    rv_free(&r);
}

/* Error paths name the field they rejected. */
static void test_errors(void) {
    Reverb r;
    int d0[NTAPS] = {0, 100, 100};
    float g[NTAPS] = {1, 1, 1};
    CK(rv_init(&r, 2, RATE, NTAPS, d0, GAIN, DECAY) == RV_ERR_DELAY);
    CK(strstr(rv_strerror(RV_ERR_DELAY), "delay") != NULL);

    int dn[NTAPS] = {-5, 100, 100};
    CK(rv_init(&r, 2, RATE, NTAPS, dn, GAIN, DECAY) == RV_ERR_DELAY);

    float gneg[NTAPS] = {1, -0.1f, 1};
    CK(rv_init(&r, 2, RATE, NTAPS, DELAY, gneg, DECAY) == RV_ERR_GAIN);
    CK(strstr(rv_strerror(RV_ERR_GAIN), "gain") != NULL);

    CK(rv_init(&r, 0, RATE, NTAPS, DELAY, GAIN, DECAY) == RV_ERR_CHANNELS);
    CK(rv_init(&r, 2, 0, NTAPS, DELAY, GAIN, DECAY) == RV_ERR_RATE);
    CK(rv_init(&r, 2, RATE, 0, DELAY, GAIN, DECAY) == RV_ERR_TAPS);
    CK(rv_init(&r, 2, RATE, NTAPS, NULL, g, DECAY) == RV_ERR_ARG);
    CK(rv_init(NULL, 2, RATE, NTAPS, DELAY, GAIN, DECAY) == RV_ERR_ARG);

    CK(make(&r, 2) == RV_OK);
    float in[8] = {0}, out[16];
    CK(rv_process(&r, in, 0, out, NULL) == RV_ERR_EMPTY);   /* empty input */
    CK(strstr(rv_strerror(RV_ERR_EMPTY), "frames") != NULL);
    CK(rv_process_legacy(&r, in, -1, out) == RV_ERR_EMPTY);
    CK(rv_process_tail(&r, in, 4, -1, out) == RV_ERR_TAIL);
    CK(rv_sum(&r, in, 0, out) == RV_ERR_EMPTY);
    CK(rv_sum(&r, out, 8, out) == RV_ERR_ALIAS);            /* in-place rejected */
    CK(rv_pad(in, 4, 2, 4, (float*)in) == RV_ERR_ALIAS);
    CK(rv_flush(&r, -1, out) == RV_ERR_TAIL);
    CK(rv_flush(&r, 0, out) == RV_OK);
    rv_free(&r);
}

/* The exported curve must match the hand table row for row. */
static void test_export(void) {
    Reverb r;
    CK(make(&r, 1) == RV_OK);
    rv_set_mix(&r, 0.0f, 1.0f);                 /* wet only, impulse response */
    enum { N = 1024 };
    float ir[N], imp[N];
    memset(imp, 0, sizeof(imp));
    imp[0] = 1.0f;

    CK(rv_sum(&r, imp, N, imp) == RV_ERR_ALIAS);    /* in-place is rejected */
    int pos_before = r.pos[0];
    CK(rv_sum(&r, imp, N, ir) == RV_OK);
    CK(r.pos[0] == (pos_before + N) % DELAY[0]);    /* state advanced by N */

    /* Explicit hand table for the first few arrival frames, in double. The
     * impulse is gain_t * fb_t^k * wet for n = (k+1) * delay_t. */
    const int probe[4] = {DELAY[0], DELAY[1], 2 * DELAY[0], 3 * DELAY[0]};
    for (int i = 0; i < 4; ++i) {
        int n = probe[i];
        double hand = 0.0;
        for (int t = 0; t < NTAPS; ++t)
            if (n % DELAY[t] == 0)
                hand += (double)GAIN[t] * pow((double)r.fb[t], (double)(n / DELAY[t] - 1));
        CK(fabs((double)ir[n] - hand) < 1e-6);
    }

    RvCurveStats st;
    CK(rv_export_curve(&r, ir, N, "rv_curve_test.csv", 1e-6, &st) == RV_OK);
    CK(st.rows == N);
    CK(st.mismatched == 0);                     /* hand table and code agree */
    CK(st.max_abs_err < 1e-6);

    /* The exported CSV itself carries the hand value and the measured value. */
    FILE *f = fopen("rv_curve_test.csv", "rb");
    CK(f != NULL);
    if (f) {
        char line[256];
        int rows = 0, header = 0;
        while (fgets(line, sizeof(line), f)) {
            if (header == 0) { header = 1; CK(strncmp(line, "frame,hand,measured", 19) == 0); continue; }
            ++rows;
        }
        fclose(f);
        CK(rows == N);
    }
    remove("rv_curve_test.csv");

    CK(rv_export_curve(&r, ir, N, "/nonexistent-dir/x.csv", 1e-6, &st) == RV_ERR_IO);
    CK(rv_export_curve(&r, ir, 0, "x.csv", 1e-6, &st) == RV_ERR_EMPTY);
    rv_free(&r);
}

int main(void) {
    test_tail_len();
    test_pad();
    test_sum_hand();
    test_legacy_equivalence();
    test_process_tail_zero();
    test_dry_passthrough();
    test_errors();
    test_export();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
