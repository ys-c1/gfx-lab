#include "mixer.h"
#include <math.h>
#include <stdio.h>
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
    return rc == 0 ? 0 : 1;
}
