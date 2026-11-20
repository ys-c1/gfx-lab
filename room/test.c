#include "room.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 手推单路梳状延迟的脉冲响应：h[(k+1)*d] = fb^k，再乘 decay 与 wet */
static float hand_computed(int frame, int d, float fb, float decay, float wet) {
    if (frame <= 0 || frame % d != 0) return 0.0f;
    int k = frame / d - 1;
    float v = decay;
    for (int i = 0; i < k; ++i) v *= fb;
    return wet * v;
}

static void test_param_errors(void) {
    Room r;
    int delay[2] = {4, 8};
    float fb[2] = {0.5f, 0.25f};
    int bad = -2;

    int bad_delay[2] = {4, 0};
    CK(room_init(&r, 2, 2, bad_delay, fb, NULL, &bad) == ROOM_ERR_DELAY);
    CK(bad == 1);
    CK(strstr(room_strerror(ROOM_ERR_DELAY), "delay") != NULL);

    float bad_fb[2] = {0.5f, 1.5f};
    bad = -2;
    CK(room_init(&r, 2, 2, delay, bad_fb, NULL, &bad) == ROOM_ERR_FEEDBACK);
    CK(bad == 1);
    CK(strstr(room_strerror(ROOM_ERR_FEEDBACK), "feedback") != NULL);

    CK(room_init(&r, 0, 2, delay, fb, NULL, NULL) == ROOM_ERR_CHANNELS);
    CK(strstr(room_strerror(ROOM_ERR_CHANNELS), "channels") != NULL);

    CK(room_init(&r, 2, 2, delay, fb, NULL, NULL) == ROOM_OK);
    float in[8] = {0}, out[8];
    CK(room_process(&r, in, out, 4, 3) == ROOM_ERR_CHANNELS); /* 声道数不符 */
    room_free(&r);
}

static void test_passthrough_bitexact(void) {
    Room r;
    int delay[2] = {4, 7};
    float fb[2] = {0.5f, 0.3f};
    CK(room_init(&r, 2, 2, delay, fb, NULL, NULL) == ROOM_OK);
    room_set_mix(&r, 1.0f, 0.0f); /* 不加混响 */

    float in[64], out[64];
    for (int i = 0; i < 64; ++i) in[i] = 0.001f * (float)((i * 37) % 23) - 0.01f;
    CK(room_process(&r, in, out, 32, 2) == ROOM_OK);
    CK(memcmp(in, out, sizeof(in)) == 0); /* 逐样本一致 */

    /* 分段路径同样逐样本一致 */
    float tap[2 * 32 * 2], wet[64], out2[64];
    CK(room_reflect(&r, in, tap, 32, 2) == ROOM_OK);
    CK(room_sum(&r, tap, wet, 32, 2) == ROOM_OK);
    CK(room_mix(&r, in, wet, out2, 32, 2) == ROOM_OK);
    CK(memcmp(in, out2, sizeof(in)) == 0);
    room_free(&r);
}

static void test_impulse_2ch(void) {
    Room r;
    int delay[1] = {4};
    float fb[1] = {0.5f}, decay[1] = {0.8f};
    CK(room_init(&r, 2, 1, delay, fb, decay, NULL) == ROOM_OK);
    room_set_mix(&r, 1.0f, 0.5f);

    float in[20 * 2] = {0}, out[20 * 2];
    in[0] = 1.0f; in[1] = 1.0f;
    CK(room_process(&r, in, out, 20, 2) == ROOM_OK);

    float maxerr = 0.0f;
    int arrival = -1;
    for (int i = 0; i < 20; ++i) {
        float expect = (i == 0) ? 1.0f : hand_computed(i, 4, 0.5f, 0.8f, 0.5f);
        for (int c = 0; c < 2; ++c) {
            float e = fabsf(out[i * 2 + c] - expect);
            if (e > maxerr) maxerr = e;
        }
        if (arrival < 0 && i > 0 && fabsf(out[i * 2]) > 0.0f) arrival = i;
    }
    CK(arrival == 4);                 /* 早期反射到达时间 = 延迟 */
    CK(fabsf(out[4 * 2] - 0.4f) < 1e-6f);  /* 手推峰值 0.5*0.8     */
    CK(fabsf(out[8 * 2] - 0.2f) < 1e-6f);  /* 0.5*0.8*0.5          */
    CK(fabsf(out[12 * 2] - 0.1f) < 1e-6f); /* 0.5*0.8*0.5^2        */
    CK(maxerr < 1e-6f);
    room_free(&r);
}

static void test_impulse_6ch(void) {
    Room r;
    int delay[1] = {4};
    float fb[1] = {0.5f}, decay[1] = {0.8f};
    CK(room_init(&r, 6, 1, delay, fb, decay, NULL) == ROOM_OK);
    room_set_mix(&r, 1.0f, 0.5f);

    float in[20 * 6], out[20 * 6];
    memset(in, 0, sizeof(in));
    for (int c = 0; c < 6; ++c) in[c] = 0.1f * (float)(c + 1); /* 每声道不同幅度 */
    CK(room_process(&r, in, out, 20, 6) == ROOM_OK);

    float maxerr = 0.0f;
    for (int c = 0; c < 6; ++c) {
        float amp = 0.1f * (float)(c + 1);
        int arrival = -1;
        for (int i = 0; i < 20; ++i) {
            float expect = (i == 0) ? amp : amp * hand_computed(i, 4, 0.5f, 0.8f, 0.5f);
            float e = fabsf(out[i * 6 + c] - expect);
            if (e > maxerr) maxerr = e;
            if (arrival < 0 && i > 0 && fabsf(out[i * 6 + c]) > 0.0f) arrival = i;
        }
        CK(arrival == 4); /* 各声道到达时间一致，无串道 */
    }
    CK(maxerr < 1e-6f);
    room_free(&r);
}

int main(void) {
    test_param_errors();
    test_passthrough_bitexact();
    test_impulse_2ch();
    test_impulse_6ch();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
