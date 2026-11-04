/*
 * test_mlpred.c -- unit tests for models/model-xgb.c / models/model-xgb-fpu.c that a CSV replay
 * cannot isolate: warm-up and window boundaries, ring wrap-around, history
 * reset, integer online scoring, output clamping and the self-test. Each
 * check compares the predictor with an independent, deliberately naive
 * reference written here (full un-ringed history, plain loops).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ML_FPU_INTERNAL
#include "../models/model-xgb.h"
#include "../models/model-xgb-fpu.h"

static int failures;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static unsigned long long rng_state = 88172645463325252ULL;
static unsigned long long rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

#define MAXT 4000
static u32 hist[MAXT][ML_N_CNT];       /* full history, hist[t][c] */
static struct ml_state st;

static u32 rand_count(int c)
{
    switch (rnd() % 8) {
    case 0: return 0;
    case 1: return (u32)(rnd() % 16);                   /* tiny */
    case 2: return 0xffffffffU - (u32)(rnd() % 4);      /* saturated */
    default:
        return (c == ML_CYC) ? (u32)(rnd() % 5000000) :
               (c == ML_INS) ? (u32)(rnd() % 12000000) : (u32)(rnd() % 300000);
    }
}

/* Naive reference of ml_build_features() for the samples ending at t. */
static void ref_features(int t, float *out)
{
    int c, i, k = 0;
    double sum[ML_N_CNT];

    for (c = 0; c < ML_N_CNT; c++) {
        double s1 = 0, s2 = 0, mn = 1e300, mx = -1e300, mn2 = 1e300;
        for (i = t - 9; i <= t; i++) {
            double v = hist[i][c];
            s1 += v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        for (i = t - 19; i <= t - 10; i++) {
            double v = hist[i][c];
            s2 += v;
            if (v < mn2) mn2 = v;
        }
        sum[c] = s1;
        out[k++] = (float)(s1 / 10.0);
        out[k++] = (float)mn;
        out[k++] = (float)mx;
        out[k++] = (float)(double)hist[t][c];
        out[k++] = (float)(s2 / 10.0);
        if (c == ML_RD || c == ML_WR)
            out[k++] = (float)mn2;
    }
    out[k++] = (float)(sum[ML_CYC] > 0 ? sum[ML_INS] / sum[ML_CYC] : 0.0);
#if ML_HAS_SL3_FRAC
    out[k++] = (float)(sum[ML_CYC] > 0 ? sum[ML_SL3] / sum[ML_CYC] : 0.0);
#endif
#if ML_HAS_SMEM_FRAC
    out[k++] = (float)(sum[ML_CYC] > 0 ? sum[ML_SMEM] / sum[ML_CYC] : 0.0);
#endif
    if (k != ML_N_FEATURES)
        printf("  FAIL reference produced %d features, ML_N_FEATURES=%d\n", k, ML_N_FEATURES), failures++;
}

static s64 ref_mibps(u32 lines)
{
    return (s64)(((unsigned long long)lines * 64000ULL) / 1048576ULL);
}

static s64 ref_to_int(float p)
{
    if (!(p > 0.0f)) return 0;
    if (p > 1.0e6f) return 1000000;
    return (s64)p;
}

/* Streams MAXT random samples with a history reset at `reset_at` (or -1)
 * and checks timing, features, outputs and the integer tracker. */
static void test_stream(int reset_at)
{
    int t, c, n_pred = 0, base = 0;
    s64 pend_rd = 0, pend_wr = 0;
    int pending = 0;
    unsigned long long abs_rd = 0, abs_wr = 0, over_rd = 0, over_wr = 0, n_eval = 0;
    int feat_bad = 0, due_bad = 0;

    ml_init(&st);
    for (t = 0; t < MAXT; t++) {
        float f[ML_N_FEATURES], r[ML_N_FEATURES], pr, pw;
        int due, want_due, n;

        for (c = 0; c < ML_N_CNT; c++)
            hist[t][c] = rand_count(c);
        if (t == reset_at) {
            ml_reset_history(&st);
            base = t;
            pending = 0;
        }
        due = ml_push(&st, hist[t]);

        n = t - base + 1;                       /* samples since the (re)start */
        if (n % 10 == 0 && pending) {           /* naive scoring of the window [t-9, t] */
            u32 mrd = 0xffffffffU, mwr = 0xffffffffU;
            int i;
            for (i = t - 9; i <= t; i++) {
                if (hist[i][ML_RD] < mrd) mrd = hist[i][ML_RD];
                if (hist[i][ML_WR] < mwr) mwr = hist[i][ML_WR];
            }
            abs_rd += (unsigned long long)llabs(pend_rd - ref_mibps(mrd));
            abs_wr += (unsigned long long)llabs(pend_wr - ref_mibps(mwr));
            over_rd += pend_rd > ref_mibps(mrd);
            over_wr += pend_wr > ref_mibps(mwr);
            n_eval++;
            pending = 0;
        }
        want_due = (n % 10 == 0) && n >= 20;
        if (due != want_due)
            due_bad++;
        if (!due)
            continue;

        ml_build_features(&st, f);
        ref_features(t, r);
        if (memcmp(f, r, sizeof(f)) != 0)
            feat_bad++;
        ml_forest(f, &pr, &pw);
        {
            s64 ir, iw;
            CHECK(ml_predict(&st, &ir, &iw) == 1, "ml_predict returned 0 on the host");
            CHECK(ir == ref_to_int(pr) && iw == ref_to_int(pw),
                  "t=%d integer output %lld/%lld vs %lld/%lld", t, (long long)ir, (long long)iw,
                  (long long)ref_to_int(pr), (long long)ref_to_int(pw));
            pend_rd = ir;
            pend_wr = iw;
            pending = 1;
        }
        n_pred++;
    }
    CHECK(due_bad == 0, "reset_at=%d: %d samples with a wrong 'prediction due'", reset_at, due_bad);
    CHECK(feat_bad == 0, "reset_at=%d: %d predictions with features != naive reference", reset_at, feat_bad);
    CHECK(st.stats.n_pred == (u32)n_pred, "n_pred %u vs %d", st.stats.n_pred, n_pred);
    CHECK(st.stats.n_eval == n_eval && st.abs_err_rd == abs_rd && st.abs_err_wr == abs_wr &&
          st.n_over_rd == over_rd && st.n_over_wr == over_wr,
          "tracker: n_eval %u/%llu abs %llu/%llu %llu/%llu over %llu/%llu %llu/%llu",
          st.stats.n_eval, n_eval, (unsigned long long)st.abs_err_rd, abs_rd,
          (unsigned long long)st.abs_err_wr, abs_wr, (unsigned long long)st.n_over_rd, over_rd,
          (unsigned long long)st.n_over_wr, over_wr);
    printf("  stream of %d samples (reset at %d): %d predictions, %llu scored, features and "
           "tracker match the naive reference\n", MAXT, reset_at, n_pred, n_eval);
}

static void test_clamp(void)
{
    CHECK(ml_pred_to_mbps(-1.0f) == 0, "negative");
    CHECK(ml_pred_to_mbps(0.0f) == 0, "zero");
    CHECK(ml_pred_to_mbps(0.99f) == 0, "truncation below 1");
    CHECK(ml_pred_to_mbps(2500.7f) == 2500, "truncation");
    CHECK(ml_pred_to_mbps(5.0e9f) == ML_PRED_MAX_MBPS, "cap");
    {
        volatile float zero = 0.0f;
        CHECK(ml_pred_to_mbps(zero / zero) == 0, "NaN");
    }
    printf("  output clamping: NaN/negative -> 0, truncation, cap at %d\n", ML_PRED_MAX_MBPS);
}

static void test_selftest(void)
{
    int rc = ml_selftest();
    CHECK(rc == 0, "ml_selftest() = %d", rc);
    printf("  ml_selftest(): %d\n", rc);
}

int main(void)
{
    printf("ML_N_CNT=%d ML_N_FEATURES=%d\n", ML_N_CNT, ML_N_FEATURES);
    test_stream(-1);
    test_stream(1234);
    test_stream(7);          /* reset before the first prediction */
    test_clamp();
    test_selftest();
    printf(failures ? "FAILED (%d)\n" : "all unit tests passed\n", failures);
    return failures ? 1 : 0;
}
