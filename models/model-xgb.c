/*
 * models/model-xgb.c -- integer-only front of the ML predictor, and the ONLY place in
 * the module that calls kernel_fpu_begin() / kernel_fpu_end().
 *
 * This object is built WITHOUT SSE (the kernel's default flags), so the
 * compiler cannot put a floating-point instruction anywhere in it; all float
 * work is in models/model-xgb-fpu.c and is reached through exactly one call that sits
 * between a begin/end pair. Rules every pair here obeys (checked by
 * host/fpu_containment_check.py):
 *
 *   - irq_fpu_usable() is tested first; if the FPU cannot be used here we do
 *     nothing floating-point and report it instead of risking corruption;
 *   - exactly ONE call between begin and end: no return/goto/break, no
 *     sleeping call, no second begin (sections must not nest);
 *   - process context only (the master kthread).
 *
 * The 1 ms path (ml_push) is integer only and never enters an FPU section;
 * an FPU section is entered once per core per 10 ms, by ml_predict().
 *
 * Built unchanged both as part of the kernel module and on the host, where
 * the FPU macros are no-ops.
 */

#ifdef __KERNEL__
#include "kernel_headers.h"
#if defined(__aarch64__) || defined(__arm__)
/* arm64/arm have no irq_fpu_usable(); may_use_simd() is the equivalent test.
 * (The ML build is verified on x86_64 only; this branch is untested.) */
#include <asm/simd.h>
#define irq_fpu_usable() may_use_simd()
#endif
#else
#include <string.h>
#define kernel_fpu_begin() do { } while (0)
#define kernel_fpu_end()   do { } while (0)
#define irq_fpu_usable()   1
#define div64_u64(a, b)    ((a) / (b))
#endif

#include "model-xgb.h"
#include "model-xgb-fpu.h"

void ml_init(struct ml_state *s)
{
    memset(s, 0, sizeof(*s));
}

void ml_reset_history(struct ml_state *s)
{
    s->head = 0;
    s->n_samples = 0;
    s->pending = 0;
    s->stats.n_resets++;
}

/* lines per 1 ms -> MiB/s, truncated (same formula as the host checks) */
static inline s64 ml_lines_to_mibps(u32 lines)
{
    return (s64)(((u64)lines * ML_LINE_MIBPS_NUM) >> ML_LINE_MIBPS_SHIFT);
}

/* Minimum of counter c over the newest ML_WINDOW samples. */
static u32 ml_window_min(const struct ml_state *s, int c)
{
    u32 i, mn = 0xffffffffU;

    for (i = 0; i < ML_WINDOW; i++) {
        u32 v = s->ring[c][(s->head + ML_HIST - 1 - i) % ML_HIST];

        if (v < mn)
            mn = v;
    }
    return mn;
}

static inline u64 ml_abs_diff(s64 a, s64 b)
{
    return (u64)(a > b ? a - b : b - a);
}

/* Integer scoring of the prediction made for the window that just ended. */
static void ml_score(struct ml_state *s)
{
    s64 real_rd = ml_lines_to_mibps(ml_window_min(s, ML_RD));
    s64 real_wr = ml_lines_to_mibps(ml_window_min(s, ML_WR));
    struct ml_stats *st = &s->stats;

    s->abs_err_rd += ml_abs_diff(s->pending_rd, real_rd);
    s->abs_err_wr += ml_abs_diff(s->pending_wr, real_wr);
    if (s->pending_rd > real_rd)
        s->n_over_rd++;
    if (s->pending_wr > real_wr)
        s->n_over_wr++;

    st->n_eval++;
    st->real_rd_mbps = real_rd;
    st->real_wr_mbps = real_wr;
    st->mae_rd_mbps = (s64)div64_u64(s->abs_err_rd, st->n_eval);
    st->mae_wr_mbps = (s64)div64_u64(s->abs_err_wr, st->n_eval);
    st->over_rd_pct = (u32)div64_u64(s->n_over_rd * 100, st->n_eval);
    st->over_wr_pct = (u32)div64_u64(s->n_over_wr * 100, st->n_eval);
    s->pending = 0;
}

int ml_push(struct ml_state *s, const u32 cnt[ML_N_CNT])
{
    int c;

    for (c = 0; c < ML_N_CNT; c++)
        s->ring[c][s->head] = cnt[c];
    s->head = (s->head + 1) % ML_HIST;
    if (s->n_samples < 0x7fffffffU)
        s->n_samples++;

    /* Windows are aligned to the start of the history. */
    if (s->n_samples % ML_WINDOW != 0)
        return 0;
    if (s->pending)
        ml_score(s);
    return ml_ready(s);
}

int ml_predict(struct ml_state *s, s64 *pred_rd_mbps, s64 *pred_wr_mbps)
{
    if (!irq_fpu_usable()) {
        s->stats.n_fpu_skipped++;
        return 0;
    }

    kernel_fpu_begin();
    ml_predict_fpu(s, pred_rd_mbps, pred_wr_mbps);
    kernel_fpu_end();

    /* Integer bookkeeping, outside the FPU section. */
    s->pending = 1;
    s->pending_rd = *pred_rd_mbps;
    s->pending_wr = *pred_wr_mbps;
    s->stats.n_pred++;
    s->stats.pred_rd_mbps = *pred_rd_mbps;
    s->stats.pred_wr_mbps = *pred_wr_mbps;
    return 1;
}

int ml_selftest(void)
{
    int rc;

    if (!irq_fpu_usable())
        return ML_SELFTEST_NO_FPU;

    kernel_fpu_begin();
    rc = ml_selftest_fpu();
    kernel_fpu_end();
    return rc;
}
