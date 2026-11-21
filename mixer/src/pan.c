#include "pan.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef MX_PAN_PI
#define MX_PAN_PI 3.14159265358979323846
#endif

const char *mx_pan_law_name(MxPanLaw law) {
    switch (law) {
    case MX_PAN_LINEAR:      return "linear";
    case MX_PAN_EQUAL_POWER: return "equal-power";
    default:                 return "invalid";
    }
}

void mx_pan_err_clear(MxPanErr *err) {
    if (!err) return;
    err->count = 0;
    err->msg[0] = '\0';
    for (int i = 0; i < MX_PAN_MAX_CONFLICTS; ++i) err->item[i][0] = '\0';
}

static int finite_f(float v) { return isfinite((double)v); }

/* 记录一条冲突：count 总会递增，item[] 只在容量内保留文本；
 * msg 由 finish_msg() 在末尾统一拼装。 */
static void add_conflict(MxPanErr *err, const char *fmt, ...) {
    int idx = err->count;
    ++err->count;
    if (idx >= MX_PAN_MAX_CONFLICTS) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->item[idx], MX_PAN_CONFLICT_LEN, fmt, ap);
    va_end(ap);
}

/* 把已记录的冲突项用 "; " 拼成 msg；超出容量的部分给出总数提示。 */
static void finish_msg(MxPanErr *err) {
    if (!err) return;
    err->msg[0] = '\0';
    int kept = err->count < MX_PAN_MAX_CONFLICTS ? err->count : MX_PAN_MAX_CONFLICTS;
    size_t used = 0;
    for (int i = 0; i < kept; ++i) {
        if (used > 0) {
            if (used + 2 >= sizeof(err->msg)) break;
            err->msg[used++] = ';';
            err->msg[used++] = ' ';
            err->msg[used] = '\0';
        }
        size_t room = sizeof(err->msg) - used - 1;
        if (room == 0) break;
        strncat(err->msg + used, err->item[i], room);
        used = strlen(err->msg);
    }
    if (err->count > MX_PAN_MAX_CONFLICTS) {
        size_t room = sizeof(err->msg) - used - 1;
        if (used > 0 && room > 2) {
            err->msg[used++] = ';';
            err->msg[used++] = ' ';
            err->msg[used] = '\0';
            room = sizeof(err->msg) - used - 1;
        }
        if (room > 0) {
            int n = snprintf(err->msg + used, room, "... %d more",
                             err->count - MX_PAN_MAX_CONFLICTS);
            (void)n;
        }
    }
}

static int layout_channels_bad(int ch) {
    return ch < 2 || ch > MX_PAN_MAX_CHANNELS || (ch % 2) != 0;
}

int mx_pan_validate(const MxPanCfg *cfg, const MixBus *b, MxPanErr *err) {
    MxPanErr local;
    MxPanErr *e = err ? err : &local;
    mx_pan_err_clear(e);

    if (!cfg || !b) {
        add_conflict(e, "conflict: %s is NULL", !cfg ? "cfg" : "bus");
        finish_msg(e);
        return e->count;
    }
    /* 关闭 = 旁路：不检查任何其他字段，与未接入时完全一致。 */
    if (cfg->enabled == 0) { finish_msg(e); return 0; }

    if (cfg->enabled != 1)
        add_conflict(e, "conflict: enabled=%d (must be 0 or 1)", cfg->enabled);
    if (cfg->law != MX_PAN_LINEAR && cfg->law != MX_PAN_EQUAL_POWER)
        add_conflict(e, "conflict: law=%d (unknown pan law)", (int)cfg->law);
    if (!finite_f(cfg->position))
        add_conflict(e, "conflict: position=%g (not finite)", (double)cfg->position);
    else if (cfg->position < -1.0f || cfg->position > 1.0f)
        add_conflict(e, "conflict: position=%g outside [-1,1]", (double)cfg->position);

    if (!b->buf)
        add_conflict(e, "conflict: bus buffer is NULL");
    if (layout_channels_bad(b->channels))
        add_conflict(e, "conflict: channels=%d (need an even count in [2,%d])",
                     b->channels, MX_PAN_MAX_CHANNELS);
    if (b->frames < 0)
        add_conflict(e, "conflict: frames=%d < 0", b->frames);
    else if (b->frames > b->cap)
        add_conflict(e, "conflict: frames=%d > cap=%d", b->frames, b->cap);

    finish_msg(e);
    return e->count;
}

int mx_pan_gains(MxPanLaw law, float position, float *gL, float *gR) {
    if (!gL || !gR) return MX_PAN_ERR_NULL;
    if (!finite_f(position) || position < -1.0f || position > 1.0f)
        return MX_PAN_ERR_INVALID;

    switch (law) {
    case MX_PAN_LINEAR:
        *gL = 0.5f * (1.0f - position);
        *gR = 0.5f * (1.0f + position);
        return MX_PAN_OK;
    case MX_PAN_EQUAL_POWER: {
        /* theta = (p+1)*pi/4 落在 [0, pi/2]。用 double 求角再落回 float，
         * 使 p=0 时两路恰好对称（同一个 sin/cos 数值）。 */
        const double theta = (double)(position + 1.0f) * (MX_PAN_PI / 4.0);
        *gL = (float)cos(theta);
        *gR = (float)sin(theta);
        return MX_PAN_OK;
    }
    default:
        return MX_PAN_ERR_INVALID;
    }
}

int mx_pan_position(MxPanLaw law, float gL, float gR, float *position) {
    if (!position) return MX_PAN_ERR_NULL;
    if (!finite_f(gL) || !finite_f(gR) || gL < 0.0f || gR < 0.0f)
        return MX_PAN_ERR_INVALID;
    if (gL == 0.0f && gR == 0.0f) return MX_PAN_ERR_INVALID;

    switch (law) {
    case MX_PAN_LINEAR: {
        const float sum = gL + gR;
        if (sum <= 0.0f) return MX_PAN_ERR_INVALID;
        *position = (gR - gL) / sum;
        return MX_PAN_OK;
    }
    case MX_PAN_EQUAL_POWER: {
        const double theta = atan2((double)gR, (double)gL); /* [0, pi/2] */
        *position = (float)(4.0 * theta / MX_PAN_PI - 1.0);
        return MX_PAN_OK;
    }
    default:
        return MX_PAN_ERR_INVALID;
    }
}

static int spread_layout_ok(const MixBus *b) {
    return b->buf && !layout_channels_bad(b->channels) &&
           b->frames >= 0 && b->frames <= b->cap;
}

int mx_pan_spread(MixBus *b, float gL, float gR, MxPanStats *stats) {
    if (stats) {
        memset(stats, 0, sizeof *stats);
        stats->law = -1;
        stats->position = NAN;
        stats->pos_est = NAN;
    }
    if (!b) return MX_PAN_ERR_NULL;
    if (!finite_f(gL) || !finite_f(gR) || gL < 0.0f || gR < 0.0f ||
        gL > 1.0f || gR > 1.0f || !spread_layout_ok(b))
        return MX_PAN_ERR_INVALID;

    /* 第二段：按增益分配到通道——每对 (2k, 2k+1) 左乘 gL、右乘 gR。 */
    const int ch = b->channels, pairs = ch / 2;
    for (int i = 0; i < b->frames; ++i) {
        float *frame = b->buf + (size_t)i * (size_t)ch;
        for (int k = 0; k < pairs; ++k) {
            frame[2 * k]     *= gL;
            frame[2 * k + 1] *= gR;
        }
    }
    if (stats) {
        stats->gL = gL;
        stats->gR = gR;
        stats->energy = gL * gL + gR * gR;
        stats->channels = ch;
        stats->pairs = pairs;
        stats->frames = b->frames;
        stats->samples = (long long)b->frames * (long long)ch;
    }
    return MX_PAN_OK;
}

int mx_pan_apply(MixBus *b, const MxPanCfg *cfg, MxPanStats *stats, MxPanErr *err) {
    MxPanErr local;
    MxPanErr *e = err ? err : &local;
    mx_pan_err_clear(e);
    if (stats) memset(stats, 0, sizeof *stats);

    if (!cfg || !b) {
        add_conflict(e, "conflict: %s is NULL", !cfg ? "cfg" : "bus");
        finish_msg(e);
        return MX_PAN_ERR_NULL;
    }
    /* 关闭：在任何校验和写入之前返回，缓冲区逐位不变。 */
    if (cfg->enabled == 0) return MX_PAN_OK;

    if (mx_pan_validate(cfg, b, e) != 0) return MX_PAN_ERR_INVALID;

    float gL = 0.0f, gR = 0.0f;
    int rc = mx_pan_gains((MxPanLaw)cfg->law, cfg->position, &gL, &gR);
    if (rc != MX_PAN_OK) {
        add_conflict(e, "conflict: gains rejected for law=%d position=%g",
                     (int)cfg->law, (double)cfg->position);
        finish_msg(e);
        return MX_PAN_ERR_INVALID;
    }

    MxPanStats sp;
    rc = mx_pan_spread(b, gL, gR, &sp);
    if (rc != MX_PAN_OK) {
        /* 校验已覆盖布局；走到这里只可能是增益异常，同样不写缓冲。 */
        add_conflict(e, "conflict: gains gL=%g gR=%g rejected",
                     (double)gL, (double)gR);
        finish_msg(e);
        return MX_PAN_ERR_INVALID;
    }
    if (stats) {
        float pos_est = NAN;
        (void)mx_pan_position((MxPanLaw)cfg->law, gL, gR, &pos_est);
        *stats = sp;
        stats->law = cfg->law;
        stats->position = cfg->position;
        stats->pos_est = pos_est;
    }
    return MX_PAN_OK;
}
