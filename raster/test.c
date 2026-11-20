#define _POSIX_C_SOURCE 200809L
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static FILE *capture_begin(int *saved) {
    FILE *f = tmpfile();
    *saved = dup(fileno(stderr));
    fflush(stderr);
    dup2(fileno(f), fileno(stderr));
    return f;
}
static void capture_end(FILE *f, int saved, char *buf, size_t cap) {
    fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    rewind(f);
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
}

static RCamTri make_cam(void) {
    RCamTri ct = {{
        { -1.5, -1.0,  2.0, 0.0, 0.0 },
        {  1.5, -1.0,  2.0, 1.0, 0.0 },
        {  0.0, -1.0, 20.0, 0.5, 1.0 },
    }};
    return ct;
}

int main(void) {
    /* ---- legacy plain rasterizer stays intact ---- */
    RTarget t;
    CK(rt_init(&t, 32, 32) == 0);
    RTri tri = {{ {4,4,0.5}, {28,4,0.5}, {16,28,0.5} }, {255, 0, 0}};
    int n = rt_triangle(&t, &tri);
    CK(n > 200);
    CK(t.z[16*32+16] < 1.0f);
    CK(t.rgb[(16*32+16)*3] == 255);
    RTri flat = {{ {0,0,0}, {8,8,0}, {16,16,0} }, {0,255,0}};
    CK(rt_triangle(&t, &flat) == -2);
    rt_free(&t);

    /* ---- shared camera-space scene at both resolutions ---- */
    const int R = 64;
    double fx = R * 0.5, fy = R * 0.5, cx = R * 0.5, cy = R * 0.5;
    RCamTri ct = make_cam();
    RTexTri tt;
    tt.rgb[0] = 200; tt.rgb[1] = 120; tt.rgb[2] = 40;
    for (int k = 0; k < 3; ++k)
        CK(rtt_project(&ct.v[k], fx, fy, cx, cy, &tt.v[k]) == RT_OK);

    /* ---- correction OFF must equal the legacy per-pixel math ---- */
    RTexTarget ta, tc;
    CK(rtt_init(&ta, R, R) == RT_OK);
    CK(rtt_init(&tc, R, R) == RT_OK);
    int na = rtt_triangle(&ta, &tt, 0);
    int nc = rtt_triangle(&tc, &tt, 1);
    CK(na > 100 && nc > 100);
    CK(na == nc);                                   /* same coverage */
    double worst_aff = 0.0, worst_cor = 0.0;
    for (int yy = 0; yy < R; ++yy) {
        for (int xx = 0; xx < R; ++xx) {
            double px = xx + 0.5, py = yy + 0.5;
            double w0 = (tt.v[2].x - tt.v[1].x) * (py - tt.v[1].y)
                      - (tt.v[2].y - tt.v[1].y) * (px - tt.v[1].x);
            double w1 = (tt.v[0].x - tt.v[2].x) * (py - tt.v[2].y)
                      - (tt.v[0].y - tt.v[2].y) * (px - tt.v[2].x);
            double w2 = (tt.v[1].x - tt.v[0].x) * (py - tt.v[0].y)
                      - (tt.v[1].y - tt.v[0].y) * (px - tt.v[0].x);
            double s = w0 + w1 + w2;
            double b0 = w0 / s, b1 = w1 / s, b2 = w2 / s;
            int inside = !(b0 < -RT_EDGE_EPS || b1 < -RT_EDGE_EPS || b2 < -RT_EDGE_EPS);
            size_t i = (size_t)yy * R + xx;
            CK(inside == (ta.z[i] < 1e29f));
            if (!inside) continue;
            /* mirror the rasterizer's shared edge clamp/renormalization */
            if (b0 < 0.0) b0 = 0.0;
            if (b1 < 0.0) b1 = 0.0;
            if (b2 < 0.0) b2 = 0.0;
            double bs = b0 + b1 + b2; b0 /= bs; b1 /= bs; b2 /= bs;
            double eu = b0 * tt.v[0].u + b1 * tt.v[1].u + b2 * tt.v[2].u;
            double ev = b0 * tt.v[0].v + b1 * tt.v[1].v + b2 * tt.v[2].v;
            double ez = b0 * tt.v[0].z + b1 * tt.v[1].z + b2 * tt.v[2].z;
            CK(ta.u[i] == eu && ta.v[i] == ev);     /* bit-identical affine */
            CK(ta.z[i] == (float)ez);

            double guv[2];
            int hit = rtt_unproject(&ct, fx, fy, cx, cy, px, py, guv);
            CK(hit == 1);
            double ea = fmax(fabs(ta.u[i] - guv[0]), fabs(ta.v[i] - guv[1]));
            double ec = fmax(fabs(tc.u[i] - guv[0]), fabs(tc.v[i] - guv[1]));
            if (ea > worst_aff) worst_aff = ea;
            if (ec > worst_cor) worst_cor = ec;
        }
    }
    CK(worst_cor < 1e-9);                 /* corrected matches inverse truth */
    CK(worst_aff > 1e-3);                 /* affine visibly warps this ground */
    CK(worst_aff > worst_cor * 1e6);

    /* inverse solve reports a miss outside the triangle */
    double guv[2];
    CK(rtt_unproject(&ct, fx, fy, cx, cy, 1.0, 1.0, guv) == 0);

    /* ---- error paths, each naming the offending vertex ---- */
    char msg[512]; int saved;
    RTexTri bad = tt;

    bad.v[2].w = 0.0;
    FILE *f = capture_begin(&saved);
    int rc = rtt_triangle(&tc, &bad, 1);
    capture_end(f, saved, msg, sizeof msg);
    CK(rc == RT_ERR_WZERO);
    CK(strstr(msg, "vertex 2") && strstr(msg, "zero depth"));

    bad = tt; bad.v[2].x = NAN;
    f = capture_begin(&saved);
    rc = rtt_triangle(&tc, &bad, 1);
    capture_end(f, saved, msg, sizeof msg);
    CK(rc == RT_ERR_FINITE);
    CK(strstr(msg, "vertex 2") && strstr(msg, "non-finite"));

    RTexTri line = tt;
    line.v[0].x = 0; line.v[0].y = 0;
    line.v[1].x = 4; line.v[1].y = 4;
    line.v[2].x = 8; line.v[2].y = 8;          /* vertex 2 on the v0-v1 line */
    f = capture_begin(&saved);
    rc = rtt_triangle(&tc, &line, 1);
    capture_end(f, saved, msg, sizeof msg);
    CK(rc == RT_ERR_DEGEN);
    CK(strstr(msg, "vertex 2") && strstr(msg, "collinear"));

    RTexTri dup = tt;
    dup.v[1] = dup.v[0];                        /* vertex 1 coincides with 0 */
    f = capture_begin(&saved);
    rc = rtt_triangle(&tc, &dup, 0);
    capture_end(f, saved, msg, sizeof msg);
    CK(rc == RT_ERR_DEGEN);
    CK(strstr(msg, "vertex 1") && strstr(msg, "coincides"));

    CK(rtt_triangle(&tc, &tt, 7) == RT_ERR_ARG);

    rtt_free(&ta);
    rtt_free(&tc);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
