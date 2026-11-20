#include "cull.h"
#include <stdlib.h>
#include <math.h>

static int overlaps(CullBox b, CullView v) {
    return !(b.x + b.w < v.x || b.x > v.x + v.w || b.y + b.h < v.y || b.y > v.y + v.h);
}

int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out) {
    if (!boxes || !out || n < 0) return -1;
    if (view.w <= 0 || view.h <= 0) return -2;
    out->ids = NULL;
    out->count = 0;
    int *ids = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!ids) return -1;
    out->ids = ids;
    for (int i = 0; i < n; ++i)
        if (overlaps(boxes[i], view)) out->ids[out->count++] = i;
    return 0;
}

int cull_count_within(const CullBox *boxes, int n, CullView view) {
    if (!boxes || n < 0) return -1;
    int c = 0;
    for (int i = 0; i < n; ++i) if (overlaps(boxes[i], view)) ++c;
    return c;
}

void cull_report_free(CullMotionReport *report) {
    if (!report) return;
    free(report->ex);
    free(report->ey);
    report->ex = report->ey = NULL;
    report->n = 0;
}

/* 阶段 1：按运动方向扩包围盒。
 * 扫掠距离 d = ratio * speed；方向归一化后单侧扩展
 *   ex_i = ratio * speed * |dx|/len,  ey_i = ratio * speed * |dy|/len
 * 向两侧各扩展该量，即把盒沿运动方向“扫”过整段轨迹。*/
int cull_expand_boxes(const CullMovingBox *boxes, int n, double ratio,
                      CullBox *expanded, CullMotionReport *report, int *err_index) {
    if (err_index) *err_index = -1;
    if (!boxes || !expanded || n < 0) return CULL_E_ARG;
    if (!(ratio > 0.0)) return CULL_E_ARG;   /* NaN/<=0：不扩展应走 cull_motion 退化路径 */

    double *ex = NULL, *ey = NULL;
    if (report) {
        ex = (double*)malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
        ey = (double*)malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
        if ((!ex || !ey) && n > 0) { free(ex); free(ey); return CULL_E_ARG; }
        report->ex = ex; report->ey = ey; report->n = n;
    }

    for (int i = 0; i < n; ++i) {
        const CullMovingBox *s = &boxes[i];
        if (!(s->speed >= 0.0)) {                 /* NaN 也一并拦截 */
            if (err_index) *err_index = i;
            return CULL_E_NEG;
        }
        double len = hypot(s->dx, s->dy);
        if (s->speed == 0.0 || !(len > 0.0)) {   /* 零向量或 NaN：需要扩展却扩展为 0 */
            if (err_index) *err_index = i;
            return CULL_E_ZERO;
        }
        double kx = fabs(s->dx) / len;
        double ky = fabs(s->dy) / len;
        double gx = ratio * s->speed * kx;        /* x 轴单侧扩展像素 */
        double gy = ratio * s->speed * ky;        /* y 轴单侧扩展像素 */
        if (report) { ex[i] = gx; ey[i] = gy; }

        CullBox *d = &expanded[i];
        d->x = s->x - gx;
        d->y = s->y - gy;
        d->w = s->w + 2.0 * gx;
        d->h = s->h + 2.0 * gy;
        d->layer = s->layer;
    }
    return CULL_OK;
}

/* 阶段 2：按扩展后盒剔除。 */
int cull_run_swept(const CullBox *expanded, int n, CullView view, CullResult *out) {
    return cull_run(expanded, n, view, out);
}

/* 两阶段合一。ratio<=0（含不扩展）：集合与旧接口完全一致，且不校验速度；
 * ratio 为 NaN：参数错误 CULL_E_ARG。 */
int cull_motion(const CullMovingBox *boxes, int n, CullView view,
                double ratio, CullResult *out,
                CullMotionReport *report, int *err_index) {
    if (err_index) *err_index = -1;
    if (report) { report->ex = report->ey = NULL; report->n = 0; }
    if (!boxes || !out || n < 0) return CULL_E_ARG;
    if (view.w <= 0 || view.h <= 0) return CULL_E_VIEW;
    if (!(ratio == ratio)) return CULL_E_ARG;   /* NaN 比例 */

    /* 退化路径：不扩展 -> 原静态剔除，逐对象集合照旧 */
    if (ratio <= 0.0) {
        CullBox *plain = (CullBox*)malloc(sizeof(CullBox) * (size_t)(n > 0 ? n : 1));
        if (!plain) return CULL_E_ARG;
        for (int i = 0; i < n; ++i) {
            plain[i].x = boxes[i].x; plain[i].y = boxes[i].y;
            plain[i].w = boxes[i].w; plain[i].h = boxes[i].h;
            plain[i].layer = boxes[i].layer;
        }
        int rc = cull_run(plain, n, view, out);
        free(plain);
        return rc == 0 ? CULL_OK : rc;
    }

    CullBox *expanded = (CullBox*)malloc(sizeof(CullBox) * (size_t)(n > 0 ? n : 1));
    if (!expanded) return CULL_E_ARG;

    int rc = cull_expand_boxes(boxes, n, ratio, expanded, report, err_index);
    if (rc != CULL_OK) { free(expanded); return rc; }

    rc = cull_run_swept(expanded, n, view, out);
    free(expanded);
    return rc == 0 ? CULL_OK : rc;
}
