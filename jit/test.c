#include "jit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static void expect_fail(int rc, int want, const char *tag) {
    ++checks;
    if (rc != want) { ++fails; printf("FAIL %s: rc=%d want=%d\n", tag, rc, want); }
    if (rc != JIT_OK) printf("  error[%s]: %s\n", tag, jit_last_error());
}

/* xorshift32 的参考实现，用于对照库内 PRNG 第一拍 */
static unsigned ref_xorshift32(unsigned x) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

int main(void) {
    /* ---------- 1. 参数校验：长度为零 / 越界要报错并指出参数 ---------- */
    JitPlan p0;
    expect_fail(jit_plan_init(&p0, 0, 100, 10, 0.5, 1), JIT_EINVAL, "N=0");
    expect_fail(jit_plan_init(&p0, 4, 0, 10, 0.5, 1), JIT_EINVAL, "src_len=0");
    expect_fail(jit_plan_init(&p0, 4, -3, 10, 0.5, 1), JIT_EINVAL, "src_len<0");
    expect_fail(jit_plan_init(&p0, 4, 100, -1, 0.5, 1), JIT_EINVAL, "period<0");
    expect_fail(jit_plan_init(&p0, 4, 100, 10, -0.5, 1), JIT_EINVAL, "amp<0");
    expect_fail(jit_plan_init(NULL, 4, 100, 10, 0.5, 1), JIT_EINVAL, "plan NULL");
    {
        const char *e = jit_last_error();
        CK(strstr(e, "plan") != NULL); /* 报错信息指出参数名 */
    }

    /* ---------- 2. PRNG 第一拍与手算一致，保证序列可复现 ---------- */
    CK(ref_xorshift32(1u) == 270369u);

    /* ---------- 3. 手算序列全链路对照 ----------
     * N=4, src_len=5, P=4, A=1.5, seed=7
     * 阶段1 连续偏移；阶段2 半数远离零取整；阶段3 等权 1/4 叠加。
     * 期望值由独立 Python 脚本按同一 xorshift32 定义逐拍推出。
     */
    JitPlan p;
    CK(jit_plan_init(&p, 4, 5, 4, 1.5, 7) == JIT_OK);
    CK(jit_out_len(&p) == (3 * 4 + 2 * 2 + 5));   /* 21 */
    CK(jit_offsets(&p) == JIT_OK);

    /* 手算 frac（12 位小数），由 xorshift32(seed=7) 逐拍推出 */
    const double want_frac[4] = {
        -1.498678207397,   /* x=1892583,    u=(x>>8)/2^24 */
         2.828563094139,   /* x=470389255                    */
         9.211689174175,   /* x=3882205507                   */
        12.644362807274    /* x=3069989445                   */
    };
    for (int k = 0; k < 4; ++k) {
        printf("  frac[%d] = %.12f (want %.12f)\n", k, p.frac[k], want_frac[k]);
        CK(fabs(p.frac[k] - want_frac[k]) < 1e-9);
    }
    CK(jit_quantize(&p) == JIT_OK);
    const int want_int[4] = {-1, 3, 9, 13};
    for (int k = 0; k < 4; ++k) {
        printf("  int [%d] = %d (want %d)\n", k, p.integer[k], want_int[k]);
        CK(p.integer[k] == want_int[k]);
    }
    CK(jit_jitter_count(&p) == 4); /* 四路取整后均偏离基准 {0,4,8,12} */

    float src[5] = {0.8f, -0.4f, 0.6f, 0.2f, -1.0f};
    int L = jit_out_len(&p);
    float *out = (float*)calloc((size_t)L, sizeof(float));
    CK(jit_synthesize(&p, src, out, L) == JIT_OK);

    /* 手算输出（ceil(A)=2，voice k 起点 = integer[k]+2，即 {1,5,11,15}） */
    float *want = (float*)calloc((size_t)L, sizeof(float));
    const int starts[4] = {1, 5, 11, 15};
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 5; ++i)
            want[starts[k] + i] += 0.25f * src[i];
    for (int i = 0; i < L; ++i) {
        printf("  out[%2d] = %+.4f (want %+.4f)\n", i, out[i], want[i]);
        CK(fabsf(out[i] - want[i]) < 1e-6f);
    }
    /* 固定锚点：voice0 的起点 out[1]=0.2；重叠区 out[11] 只有 voice2=0.2 */
    CK(fabsf(out[1] - 0.2f) < 1e-6f);
    CK(fabsf(out[0] - 0.0f) < 1e-6f);
    /* 容量不足应报错并指出 cap */
    expect_fail(jit_synthesize(&p, src, out, L - 1), JIT_ERANGE, "cap too small");
    CK(strstr(jit_last_error(), "cap") != NULL);
    free(out); free(want);

    /* ---------- 4. 取整规则：半数远离零（含 .5 打结）+ 越界 ---------- */
    JitPlan pt;
    /* amp=3 使包络为 [-3,11]，下面的取整值全部合法 */
    CK(jit_plan_init(&pt, 5, 4, 2, 3.0, 1) == JIT_OK);
    CK(jit_offsets(&pt) == JIT_OK);
    pt.frac[0] =  0.5;  pt.frac[1] = -0.5;
    pt.frac[2] =  2.4999; pt.frac[3] = 2.5001; pt.frac[4] = -2.5;
    CK(jit_quantize(&pt) == JIT_OK);
    CK(pt.integer[0] == 1 && pt.integer[1] == -1);
    CK(pt.integer[2] == 2 && pt.integer[3] == 3 && pt.integer[4] == -3);
    /* 手动制造越界偏移：超出包络 [-3,11] */
    pt.frac[2] = 12.7;
    expect_fail(jit_quantize(&pt), JIT_ERANGE, "offset OOB");
    CK(strstr(jit_last_error(), "out of range") != NULL);
    jit_plan_free(&pt);

    /* ---------- 5. 不做抖动时逐样本一致（等价普通重复叠加） ---------- */
    JitPlan pn;
    CK(jit_plan_init(&pn, 4, 6, 3, 0.0, 1) == JIT_OK);
    CK(jit_offsets(&pn) == JIT_OK && jit_quantize(&pn) == JIT_OK);
    CK(jit_jitter_count(&pn) == 0);
    float sn[6] = {0.5f, -0.25f, 0.125f, 1.0f, -0.75f, 0.3f};
    int Ln = jit_out_len(&pn);
    CK(Ln == 3 * 3 + 6);
    float *on = (float*)calloc((size_t)Ln, sizeof(float));
    CK(jit_synthesize(&pn, sn, on, Ln) == JIT_OK);
    /* 朴素参考：每路落在同一基准点 k*3 */
    float *ref = (float*)calloc((size_t)Ln, sizeof(float));
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 6; ++i) ref[k * 3 + i] += 0.25f * sn[i];
    for (int i = 0; i < Ln; ++i) {
        CK(fabsf(on[i] - ref[i]) < 2e-7f); /* 逐样本与朴素参考一致（容浮点序误差） */
    }
    /* 锚点：首个样本只被一路覆盖，恰为 0.25*sn[0]；index 3 为 voice0/voice1 重叠点 */
    CK(fabsf(on[0] - 0.25f * sn[0]) < 1e-6f);
    CK(fabsf(on[3] - 0.25f * (sn[3] + sn[0])) < 1e-6f);
    free(on); free(ref);
    jit_plan_free(&pn);

    /* ---------- 6. 确定性：同种子序列完全相同 ---------- */
    JitPlan pa, pb;
    CK(jit_plan_init(&pa, 16, 32, 8, 2.0, 12345) == JIT_OK);
    CK(jit_plan_init(&pb, 16, 32, 8, 2.0, 12345) == JIT_OK);
    CK(jit_offsets(&pa) == JIT_OK && jit_offsets(&pb) == JIT_OK);
    for (int k = 0; k < 16; ++k) CK(pa.frac[k] == pb.frac[k]);
    /* 所有抖动都落在 [-A, A] */
    for (int k = 0; k < 16; ++k) {
        double j = pa.frac[k] - k * 8.0;
        CK(j >= -2.0 - 1e-12 && j <= 2.0 + 1e-12);
    }
    jit_plan_free(&pa); jit_plan_free(&pb);

    /* ---------- 7. CSV 导出往返 ---------- */
    JitPlan pe;
    CK(jit_plan_init(&pe, 6, 10, 5, 1.0, 99) == JIT_OK);
    CK(jit_offsets(&pe) == JIT_OK && jit_quantize(&pe) == JIT_OK);
    FILE *fp = fopen("build/_seq.csv", "w");
    CK(fp != NULL);
    CK(jit_export_csv(&pe, fp) == JIT_OK);
    fclose(fp);
    fp = fopen("build/_seq.csv", "r");
    CK(fp != NULL);
    char hdr[128];
    CK(fgets(hdr, sizeof(hdr), fp) != NULL);
    CK(strncmp(hdr, "voice,base,frac,", 16) == 0);
    int rows = 0, voice, jq, iv; double base, frac, jit, w;
    while (fscanf(fp, "%d,%lf,%lf,%lf,%d,%d,%lf",
                  &voice, &base, &frac, &jit, &jq, &iv, &w) == 7) {
        CK(voice == rows);
        CK(fabs(frac - pe.frac[rows]) < 1e-12);
        CK(iv == pe.integer[rows]);
        CK(fabs(w - 1.0 / 6.0) < 1e-12);
        ++rows;
    }
    CK(rows == 6);
    fclose(fp);
    remove("build/_seq.csv");
    jit_plan_free(&pe);

    /* ---------- 8. FFT 解析解对照：两个等权脉冲间距 4 ----------
     * 脉冲响应 [1,0,0,0,1,0,0,0]（权重 1/2，故谱峰幅度 1.0）：
     * 偶数 bin 幅度 1.0，奇数 bin 为 0；峰均比 20log10(1/0.5)=6.0206 dB。
     */
    JitPlan pf;
    CK(jit_plan_init(&pf, 2, 1, 4, 0.0, 1) == JIT_OK);
    CK(jit_offsets(&pf) == JIT_OK && jit_quantize(&pf) == JIT_OK);
    JitCombMetric mf;
    CK(jit_comb_metric(&pf, &mf) == JIT_OK);
    printf("  two-pulse: nfft=%d crest=%.4f dB peak_bin=%d above_half=%d flatness=%.5f\n",
           mf.nfft, mf.crest_db, mf.peak_bin, mf.above_half, mf.flatness);
    CK(mf.nfft == 8 && mf.bins == 4);
    CK(fabs(mf.crest_db - 20.0 * log10(2.0)) < 1e-9);
    CK(mf.peak_bin == 2);
    CK(mf.above_half == 2);
    jit_plan_free(&pf);

    /* ---------- 9. 梳状指标：单脉冲平直、周期有梳齿、抖动削弱梳齿 ---------- */
    JitPlan ps;
    CK(jit_plan_init(&ps, 1, 16, 4, 0.0, 1) == JIT_OK);
    CK(jit_offsets(&ps) == JIT_OK && jit_quantize(&ps) == JIT_OK);
    JitCombMetric m1;
    CK(jit_comb_metric(&ps, &m1) == JIT_OK);
    CK(fabs(m1.crest_db - 0.0) < 1e-9);       /* 单脉冲频谱平直 */
    CK(m1.above_half == m1.bins);             /* 所有 bin 同为峰值 */
    CK(fabs(m1.flatness - 1.0) < 1e-9);
    jit_plan_free(&ps);

    JitPlan pc;
    CK(jit_plan_init(&pc, 16, 32, 8, 0.0, 1) == JIT_OK);   /* 严格周期 */
    CK(jit_offsets(&pc) == JIT_OK && jit_quantize(&pc) == JIT_OK);
    JitCombMetric mc;
    CK(jit_comb_metric(&pc, &mc) == JIT_OK);
    printf("  periodic: crest=%.2f dB peak_bin=%d above_half=%d flatness=%.5f\n",
           mc.crest_db, mc.peak_bin, mc.above_half, mc.flatness);
    CK(mc.crest_db > 12.0);
    CK(mc.flatness < 0.2);
    CK(mc.above_half <= mc.bins / 8);        /* 梳齿稀疏 */

    JitPlan pj;
    CK(jit_plan_init(&pj, 16, 32, 8, 3.0, 1) == JIT_OK);   /* 抖动 */
    CK(jit_offsets(&pj) == JIT_OK && jit_quantize(&pj) == JIT_OK);
    JitCombMetric mj;
    CK(jit_comb_metric(&pj, &mj) == JIT_OK);
    printf("  jittered: crest=%.2f dB peak_bin=%d above_half=%d flatness=%.5f\n",
           mj.crest_db, mj.peak_bin, mj.above_half, mj.flatness);
    CK(mj.crest_db < mc.crest_db);           /* 抖动削弱梳齿 */
    CK(mj.flatness > mc.flatness);
    CK(mj.above_half >= mc.above_half);
    CK(jit_jitter_count(&pj) > 0);

    /* 阶段顺序错误也要报错 */
    JitPlan px;
    CK(jit_plan_init(&px, 3, 8, 4, 0.5, 1) == JIT_OK);
    expect_fail(jit_quantize(&px), JIT_EINVAL, "stage order");
    float dummy[8] = {0};
    expect_fail(jit_synthesize(&px, dummy, NULL, 0), JIT_EINVAL, "stage order synth");
    jit_plan_free(&px);
    jit_plan_free(&pc);
    jit_plan_free(&pj);
    jit_plan_free(&p);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
