/*
 * probe_main.c -- ml_fpu_probe.ko: exercises ONLY the ML predictor's floating-
 * point path, so it can be validated on a given kernel in isolation, before
 * the real regulator (areg.ko) is ever loaded.
 *
 * It creates no perf events, no threads, no debugfs entries and throttles
 * nothing. On insmod (process context) it:
 *   1. runs ml_selftest() once -- arithmetic + generated-model sanity inside
 *      a single kernel_fpu_begin()/kernel_fpu_end() section;
 *   2. repeats it `iterations` times (cond_resched() between sections, never
 *      inside one) and reports the time per section;
 *   3. drives the real entry points with 45 synthetic 1 ms samples:
 *      ml_push() (integer) every sample, ml_predict() (one FPU section)
 *      whenever a prediction is due -- exactly what the master thread does --
 *      and reports the slowest ml_predict().
 * If any step fails the module refuses to load (-EIO); nothing stays resident.
 *
 * Build:   make probe          (produces ml_fpu_probe.ko)
 * Use:     sudo insmod ml_fpu_probe.ko ; dmesg | tail ; sudo rmmod ml_fpu_probe
 *
 * This object is built WITHOUT SSE like every file except models/model-xgb-fpu.c, so even
 * this probe cannot execute a floating-point instruction outside a section.
 */
#include "kernel_headers.h"
#include "models/model-xgb.h"

static int iterations = 1000;
module_param(iterations, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(iterations, "number of self-test repetitions (default 1000)");

static struct ml_state probe_state;     /* static: keep the ring off the stack */

#define PROBE_SAMPLES 45                /* -> predictions at samples 20, 30, 40 */

static int __init probe_init(void)
{
    s64 pred_rd = 0, pred_wr = 0;
    u64 t0, t1, worst = 0;
    u32 cnt[ML_N_CNT];
    int i, c, rc, due = 0;

    pr_info("FPU probe: starting (no perf events, no threads, no throttling)\n");

    rc = ml_selftest();
    if (rc) {
        pr_err("self-test FAILED, code %d%s\n", rc,
               rc == ML_SELFTEST_NO_FPU ? " (irq_fpu_usable() is false here)" : "");
        return -EIO;
    }
    pr_info("self-test passed\n");

    if (iterations < 1)
        iterations = 1;
    t0 = ktime_get_ns();
    for (i = 0; i < iterations; i++) {
        rc = ml_selftest();
        if (rc) {
            pr_err("self-test FAILED on repetition %d, code %d\n", i, rc);
            return -EIO;
        }
        cond_resched();                 /* between FPU sections, never inside one */
    }
    t1 = ktime_get_ns();
    pr_info("%d self-tests passed, %llu ns per FPU section\n",
            iterations, div64_u64(t1 - t0, (u64)iterations));

    /* The real entry points, with synthetic 1 ms samples. */
    ml_init(&probe_state);
    for (i = 0; i < PROBE_SAMPLES; i++) {
        for (c = 0; c < ML_N_CNT; c++)
            cnt[c] = 1000 + 37 * (u32)i + 101 * (u32)c;
        cnt[ML_CYC] = 3000000;
        cnt[ML_INS] = 4000000 + 1000 * (u32)i;
        if (ml_push(&probe_state, cnt)) {
            due++;
            t0 = ktime_get_ns();
            if (!ml_predict(&probe_state, &pred_rd, &pred_wr)) {
                pr_err("ml_predict skipped: FPU unusable in process context?\n");
                return -EIO;
            }
            t1 = ktime_get_ns();
            if (t1 - t0 > worst)
                worst = t1 - t0;
        }
        cond_resched();
    }
    pr_info("ml_push/ml_predict: %u predictions from %d samples, last forecast read=%lld "
            "write=%lld MiB/s, slowest ml_predict %llu ns, scored=%u\n",
            probe_state.stats.n_pred, PROBE_SAMPLES, pred_rd, pred_wr, worst,
            probe_state.stats.n_eval);
    if (due != 3 || probe_state.stats.n_pred != 3 || probe_state.stats.n_eval != 2) {
        pr_err("expected 3 predictions and 2 scored windows\n");
        return -EIO;
    }

    pr_info("FPU probe OK. Safe to load areg.ko (keep ml_enable=0 for the first run).\n");
    return 0;
}

static void __exit probe_exit(void)
{
    pr_info("FPU probe unloaded\n");
}

module_init(probe_init);
module_exit(probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("isolated FPU-path probe for the LMS-AR ML predictor");
