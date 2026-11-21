#include "cull.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static CullBox mkbox(double x, double y, double w, double h,
                     double vx, double vy, double tol) {
    CullBox b = {x, y, w, h, 0, vx, vy, tol};
    return b;
}

int main(void) {
    /* ---- legacy behaviour unchanged -------------------------------- */
    CullBox b[4] = {
        {0, 0, 10, 10, 0, 0.0, 0.0, 0.0},
        {100, 100, 10, 10, 1, 0.0, 0.0, 0.0},
        {50, 50, 20, 20, 0, 0.0, 0.0, 0.0},
        {-5, -5, 10, 10, 2, 0.0, 0.0, 0.0},
    };
    CullView v = {0, 0, 60, 60};
    CullResult r;
    CK(cull_run(b, 4, v, &r) == 0);
    CK(r.count == 3);
    CK(r.ids[0] == 0 && r.ids[1] == 2 && r.ids[2] == 3);
    CK(cull_count_within(b, 4, v) == 3);
    CullView bad = {0, 0, 0, 10};
    CK(cull_run(b, 4, bad, &r) == -2);
    free(r.ids);

    /* ---- hand-computed two-stage cases, view [0,60]x[0,60], dt=0.1 - */
    enum { HN = 8 };
    CullBox h[HN] = {
        mkbox(  0,  0, 10, 10,    0,    0, 0), /* 0 static inside        */
        mkbox(100,100, 10, 10,    0,    0, 0), /* 1 far away              */
        mkbox( 50, 50, 20, 20,    0,    0, 0), /* 2 overlaps right/top    */
        mkbox( -5, -5, 10, 10,    0,    0, 0), /* 3 overlaps corner       */
        mkbox( 65, 10, 10, 10, -100,    0, 0), /* 4 enters from right     */
        mkbox( 65, 30, 10, 10,  -10,    0, 0), /* 5 too slow, stays out   */
        mkbox( 10, 65, 10, 10,    0, -100, 0), /* 6 enters from top       */
        mkbox( 65, 65, 10, 10,  -30,  -40, 0), /* 7 diagonal, conservative*/
    };
    CullParams p = {0.1, 0.0};
    double tols[HN];
    CullError err = {0, -1, 0, 0};
    CK(cull_apply_tolerance(h, HN, p, tols, &err) == 0);
    /* tol = hypot(vx,vy)*dt */
    CK(tols[0] == 0.0 && tols[1] == 0.0 && tols[2] == 0.0 && tols[3] == 0.0);
    CK(fabs(tols[4] - 10.0) < 1e-12);
    CK(fabs(tols[5] - 1.0) < 1e-12);
    CK(fabs(tols[6] - 10.0) < 1e-12);
    CK(fabs(tols[7] - 5.0) < 1e-12);   /* hypot(30,40)=50, *0.1 */

    CullResult legacy, adapt;
    CK(cull_run(h, HN, v, &legacy) == 0);
    CK(legacy.count == 3);
    CK(legacy.ids[0] == 0 && legacy.ids[1] == 2 && legacy.ids[2] == 3);

    CK(cull_run_adaptive(h, HN, v, p, tols, &adapt, &err) == 0);
    CK(adapt.count == 6);
    CK(adapt.ids[0] == 0 && adapt.ids[1] == 2 && adapt.ids[2] == 3 &&
       adapt.ids[3] == 4 && adapt.ids[4] == 6 && adapt.ids[5] == 7);
    /* boxes 4 and 6 genuinely reach the view within dt:
       b4 next frame x = 65-10 = 55 <= 60; b6 next frame y = 55 <= 60 */
    CK(65.0 + h[4].vx * p.dt == 55.0);
    CK(65.0 + h[6].vy * p.dt == 55.0);
    /* box 5 grown by 1 px still leaves a 4 px gap: culled either way */
    CK(65.0 - tols[5] > 60.0);
    free(legacy.ids);
    free(adapt.ids);

    /* ---- no expansion => identical set to legacy ------------------- */
    srand(1234);
    enum { RN = 200 };
    CullBox rb[RN];
    for (int i = 0; i < RN; ++i) {
        rb[i] = mkbox((rand() % 200) - 50.0, (rand() % 200) - 50.0,
                      1 + rand() % 20, 1 + rand() % 20,
                      (rand() % 401) - 200.0, (rand() % 401) - 200.0, 0.0);
        rb[i].layer = i % 3;
    }
    double rtols[RN];
    /* dt = 0: every tolerance collapses to 0 despite nonzero velocity */
    CullParams p0 = {0.0, 0.0};
    CK(cull_apply_tolerance(rb, RN, p0, rtols, 0) == 0);
    for (int i = 0; i < RN; ++i) CK(rtols[i] == 0.0);
    CullResult rl, ra;
    CK(cull_run(rb, RN, v, &rl) == 0);
    CK(cull_run_adaptive(rb, RN, v, p0, rtols, &ra, 0) == 0);
    CK(rl.count == ra.count);
    for (int i = 0; i < rl.count; ++i) CK(rl.ids[i] == ra.ids[i]);
    free(rl.ids);
    free(ra.ids);

    /* ---- viewport margin alone keeps edge objects ------------------ */
    double zt[HN] = {0};
    CullParams pm = {0.0, 5.0};
    CullResult rm;
    CK(cull_run_adaptive(h, HN, v, pm, zt, &rm, 0) == 0);
    /* b5 x0=65 touches margin-grown right edge 60+5=65 -> now kept */
    int has5 = 0;
    for (int i = 0; i < rm.count; ++i) if (rm.ids[i] == 5) has5 = 1;
    CK(has5);
    free(rm.ids);

    /* ---- error paths: field names and object index ----------------- */
    CullBox eb[2] = {
        mkbox(0, 0, 10, 10, 0, 0, 0),
        mkbox(0, 0, 10, 10, 0, 0, 0),
    };
    double et[2] = {0, 0};

    eb[1].tol = -0.5;
    err = (CullError){0, -1, 0, 0};
    CK(cull_apply_tolerance(eb, 2, p, et, &err) == -3);
    CK(err.index == 1 && err.field && strcmp(err.field, "tol") == 0);
    eb[1].tol = 0;

    eb[0].vx = INFINITY;
    err = (CullError){0, -1, 0, 0};
    CK(cull_apply_tolerance(eb, 2, p, et, &err) == -3);
    CK(err.index == 0 && err.field && strcmp(err.field, "vx/vy") == 0);
    eb[0].vx = 0;

    eb[1].vy = NAN;
    err = (CullError){0, -1, 0, 0};
    CK(cull_run_adaptive(eb, 2, v, p, et, &r, &err) == -3);
    CK(err.index == 1 && err.field && strcmp(err.field, "vx/vy") == 0);
    eb[1].vy = 0;

    CullParams pdt = {-1.0, 0.0};
    err = (CullError){0, -1, 0, 0};
    CK(cull_apply_tolerance(eb, 2, pdt, et, &err) == -3);
    CK(err.index == -1 && err.field && strcmp(err.field, "dt") == 0);

    CullParams pmg = {0.1, -2.0};
    err = (CullError){0, -1, 0, 0};
    CK(cull_run_adaptive(eb, 2, v, pmg, et, &r, &err) == -3);
    CK(err.index == -1 && err.field && strcmp(err.field, "margin") == 0);

    et[0] = -7.0;
    err = (CullError){0, -1, 0, 0};
    CK(cull_run_adaptive(eb, 2, v, p, et, &r, &err) == -3);
    CK(err.index == 0 && err.field && strcmp(err.field, "tolerance") == 0);
    et[0] = 0.0;
    eb[1].vy = NAN; /* re-arm: restored above for the named-error check */

    /* null error pointer must not crash */
    CK(cull_apply_tolerance(eb, 2, pdt, et, 0) == -3);
    CK(cull_run_adaptive(eb, 2, v, p, et, &r, 0) == -3);
    eb[1].vy = 0.0;

    /* ---- candidate list export round trip -------------------------- */
    CK(cull_apply_tolerance(h, HN, p, tols, 0) == 0);
    CK(cull_run_adaptive(h, HN, v, p, tols, &adapt, 0) == 0);
    char path[128];
    snprintf(path, sizeof path, "/tmp/cull_export_%d.txt", (int)getpid());
    CK(cull_export(path, &adapt) == 0);
    FILE *f = fopen(path, "r");
    CK(f != 0);
    int id, got = 0;
    while (fscanf(f, "%d", &id) == 1) {
        CK(got < adapt.count && id == adapt.ids[got]);
        ++got;
    }
    CK(got == adapt.count);
    fclose(f);
    remove(path);
    free(adapt.ids);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
