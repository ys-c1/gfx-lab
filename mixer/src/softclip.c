#include "softclip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* 查找表覆盖 [0, knee * SC_SPAN]; 超出表尾的样本直接走解析尾部, 永不硬切。 */
#define SC_SPAN 8.0f

float sc_analytic(float knee, float ceiling, float x) {
    if (!(knee > 0.0f) || !(ceiling >= knee)) return 0.0f;
    float ax = fabsf(x);
    float y;
    if (ax <= knee) {
        y = ax;                                        /* 线性区 (含拐点) */
    } else if (ceiling > knee) {
        float d = ceiling - knee;                      /* 指数尺度: 保证 f'(T)=1 */
        y = ceiling - d * expf(-(ax - knee) / d);
    } else {
        y = knee;                                      /* C == T: 退化为硬切 */
    }
    return x < 0.0f ? -y : y;                          /* 奇对称 */
}

int sc_build_curve(SoftClip *sc, float knee, float ceiling, int tabs,
                   unsigned *conflicts) {
    if (!sc) return -1;
    sc->valid = 0; sc->tab = NULL;

    unsigned bad = 0;
    if (!isfinite(knee))          bad |= SC_CONF_KNEE_NANINF;
    if (!isfinite(ceiling))       bad |= SC_CONF_CEIL_NANINF;
    if (isfinite(knee) && knee <= 0.0f)
        bad |= SC_CONF_KNEE_NONPOS;
    if (isfinite(ceiling) && ceiling <= 0.0f)
        bad |= SC_CONF_CEIL_NONPOS;
    if (isfinite(knee) && isfinite(ceiling) && ceiling < knee)
        bad |= SC_CONF_CEIL_LT_KNEE;
    if (tabs < 4)                 bad |= SC_CONF_TABS_SMALL;
    if (bad) {
        if (conflicts) *conflicts = bad;
        return -2;
    }

    float *tab = (float*)malloc((size_t)tabs * sizeof(float));
    if (!tab) return -1;
    for (int i = 0; i < tabs; ++i) {
        float x = knee * SC_SPAN * (float)i / (float)(tabs - 1);
        tab[i] = sc_analytic(knee, ceiling, x);
    }
    sc->knee = knee; sc->ceiling = ceiling; sc->tabs = tabs;
    sc->tab = tab; sc->valid = 1;
    if (conflicts) *conflicts = 0;
    return 0;
}

void sc_free(SoftClip *sc) {
    if (!sc) return;
    free(sc->tab);
    sc->tab = NULL; sc->valid = 0; sc->tabs = 0;
}

long sc_apply(const SoftClip *sc, float *buf, int frames, int channels) {
    if (!buf || frames <= 0 || channels <= 0) return -1;
    long n = (long)frames * channels;
    if (!sc || !sc->valid) return n;           /* 未启用: 直通, 与原行为一致 */

    const float T = sc->knee, C = sc->ceiling;
    const int tabs = sc->tabs;
    const float *tab = sc->tab;
    const float scale = (float)(tabs - 1) / SC_SPAN;

    for (long i = 0; i < n; ++i) {
        float x = buf[i];
        float ax = fabsf(x);
        if (ax <= T) continue;                 /* 拐点(含)以下位级原样保留 */
        if (C <= T) { buf[i] = x < 0.0f ? -T : T; continue; } /* 硬切退化 */

        float y;
        float pos = ax / T;                    /* 以拐点为单位的位置 */
        float fi = pos * scale;
        int idx = (int)fi;
        if (idx >= tabs - 1) {
            /* 表尾之外: 解析尾部, 渐近收敛到 ceiling, 不钳位 */
            float d = C - T;
            y = C - d * expf(-(ax - T) / d);
        } else {
            float frac = fi - (float)idx;
            y = tab[idx] + frac * (tab[idx + 1] - tab[idx]);
        }
        buf[i] = x < 0.0f ? -y : y;
    }
    return n;
}

int sc_export_csv(const SoftClip *sc, const char *path, int n, float xmax) {
    if (!sc || !sc->valid || !path || n < 2 || !(xmax > 0.0f)) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -2;
    fprintf(f, "input,analytic,lut\n");
    const float T = sc->knee, C = sc->ceiling;
    const int tabs = sc->tabs;
    const float scale = (float)(tabs - 1) / SC_SPAN;

    for (int i = 0; i < n; ++i) {
        float x = -xmax + 2.0f * xmax * (float)i / (float)(n - 1);
        float ya = sc_analytic(T, C, x);

        float ax = fabsf(x);
        float yl;
        if (ax <= T) {
            yl = ax;
        } else if (C <= T) {
            yl = T;
        } else {
            float fi = (ax / T) * scale;
            int idx = (int)fi;
            if (idx >= tabs - 1) {
                float d = C - T;
                yl = C - d * expf(-(ax - T) / d);
            } else {
                float frac = fi - (float)idx;
                yl = sc->tab[idx] + frac * (sc->tab[idx + 1] - sc->tab[idx]);
            }
        }
        if (x < 0.0f) yl = -yl;       /* 奇对称: 符号统一在分支外处理 */
        fprintf(f, "%.9g,%.9g,%.9g\n", x, ya, yl);
    }
    fclose(f);
    return 0;
}
