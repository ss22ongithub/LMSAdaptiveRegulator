/*
 * models/model-xgb.h -- streaming "next-10 ms bandwidth floor" predictor for LMS-AR.
 *
 * Replaces the LMS estimator. Every 1 ms the master thread pushes ONE sample
 * per regulated core (the core's counter deltas, normalised to exactly 1 ms);
 * every 10th sample, once 20 consecutive samples exist, it predicts the
 * minimum per-ms read / write bandwidth of that core over the NEXT 10 ms.
 * That is xgboost_mem_next_1ms from simpleML_memguard (xgboost_mem_1ms.py,
 * features in bw1ms_features.py), converted to C with emlearn
 * (see models/xgb/).
 *
 * Units. "Bandwidth" is per-core calc bandwidth: Read = cache-misses x 64 B,
 * Write = LLC-store-misses x 64 B. Predictions are MiB/s, the unit of the
 * regulator's budgets (utils.c convert_events_to_mb).
 *
 * Floating point -- how it is kept away from the rest of the kernel
 * -----------------------------------------------------------------
 * The Linux kernel does not save/restore SSE state on kernel entry. Any SSE
 * instruction executed outside a kernel_fpu_begin()/kernel_fpu_end() section
 * silently corrupts the FPU registers of whichever user process was running,
 * and doing it in the wrong context (IRQ, sleeping, nested) can hang or
 * crash the machine. So the design is:
 *
 *   models/model-xgb.c   (this API; built WITHOUT SSE)
 *       ml_init(), ml_push(),    plain integer code: the 1 ms path never
 *       ml_reset_history()       touches the FPU
 *       ml_predict(), ml_selftest()  check irq_fpu_usable(), then
 *                                 kernel_fpu_begin(); <ONE call>; kernel_fpu_end();
 *   models/model-xgb-fpu.c   (all the float/double work; the ONLY object built WITH SSE)
 *       reachable only from the call between begin and end. See models/model-xgb-fpu.h.
 *
 * The state below holds integers only, so no object other than models/model-xgb-fpu.o
 * ever stores a float. ml_predict()/ml_selftest() need process context
 * (the master kthread) and must not be called from IRQ/NMI/softirq
 * context. Nothing inside the FPU section sleeps.
 */
#ifndef MODEL_XGB_H
#define MODEL_XGB_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  s64;
#endif

/*
 * Model inputs, one count per 1 ms sample, in the order of the model's
 * feature columns (xgboost_mem_1ms.py --counters ...):
 *   X(enum id, perf name, perf type, perf config)
 * The perf type/config are only expanded in ar.c (kernel build).
 * All six are generic perf events, so the kernel picks the same hardware
 * event as `perf stat -e <name>` did when the data was recorded (no raw
 * event encodings to get wrong). PMU budget per core: cycles / instructions
 * use fixed counters, the other four one general-purpose counter each
 * (LLC-load/store-misses also use the two OFFCORE_RSP MSRs) -- see
 * README_ML.md. Chosen by greedy forward selection (NOTES_1ms_model3.md).
 */
#define ML_COUNTER_LIST(X)                                                       \
    X(ML_RD,   "cache-misses",      PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES) \
    X(ML_WR,   "LLC-store-misses",  PERF_TYPE_HW_CACHE,                          \
      PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) |             \
      (PERF_COUNT_HW_CACHE_RESULT_MISS << 16))                                   \
    X(ML_CYC,  "cycles",            PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES)   \
    X(ML_INS,  "instructions",      PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS) \
    X(ML_LD,   "LLC-load-misses",   PERF_TYPE_HW_CACHE,                          \
      PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) |              \
      (PERF_COUNT_HW_CACHE_RESULT_MISS << 16))                                   \
    X(ML_CREF, "cache-references",  PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES)

#define ML_ENUM_ENTRY(id, name, type, config) id,
enum ml_counter { ML_COUNTER_LIST(ML_ENUM_ENTRY) ML_N_CNT };
#undef ML_ENUM_ENTRY

/* Ratio features present in the model (sums over the last window / cycles). */
#define ML_HAS_SL3_FRAC     0       /* needs an ML_SL3 counter  */
#define ML_HAS_SMEM_FRAC    0       /* needs an ML_SMEM counter */

/* Must match the training data (bw1ms_features.py). */
#define ML_SAMPLE_NS        1000000ULL          /* 1 ms samples */
#define ML_DT_MIN_NS        900000ULL           /* valid sample: 0.9 .. 2 ms */
#define ML_DT_MAX_NS        2000000ULL
#define ML_WINDOW           10                  /* 10 ms windows */
#define ML_HIST             (2 * ML_WINDOW)     /* inputs: the last 2 windows */

/* 5 features per counter (mean, min, max, last of the last window; mean of
 * the one before) + min of the one before for read / write, + ipc + the
 * optional stall fractions. */
#define ML_N_FEATURES       (5 * ML_N_CNT + 2 + 1 + ML_HAS_SL3_FRAC + ML_HAS_SMEM_FRAC)

/* lines per 1 ms -> MiB/s is x 64 * 1000 / 2^20 */
#define ML_LINE_MIBPS_NUM   64000ULL
#define ML_LINE_MIBPS_SHIFT 20

#define ML_SELFTEST_NO_FPU  (-100)           /* irq_fpu_usable() was false */

/* Integer view of the predictor (debugfs). All bandwidths in MiB/s. */
struct ml_stats {
    u32 n_pred;                 /* predictions produced */
    u32 n_eval;                 /* predictions already scored against reality */
    u32 n_fpu_skipped;          /* predictions skipped: FPU unusable */
    u32 n_resets;               /* history restarts (gaps) */
    s64 pred_rd_mbps;           /* latest prediction: floor of the next 10 ms */
    s64 pred_wr_mbps;
    s64 real_rd_mbps;           /* realised floor of the latest scored window */
    s64 real_wr_mbps;
    s64 mae_rd_mbps;            /* running mean absolute error */
    s64 mae_wr_mbps;
    u32 over_rd_pct;            /* % of scored predictions above the realised floor */
    u32 over_wr_pct;            /* (the QoS-risk direction) */
};

struct ml_state {
    u32 ring[ML_N_CNT][ML_HIST];    /* counts per 1 ms sample */
    u32 head;                       /* ring slot the next sample is written to */
    u32 n_samples;                  /* consecutive samples since the last reset */

    /* Prediction for the window now in progress, scored (integer) when it
     * ends. Clamped >= 0, MiB/s. */
    u32 pending;
    s64 pending_rd, pending_wr;

    /* Online accuracy sums (MiB/s) */
    u64 n_over_rd, n_over_wr;
    u64 abs_err_rd, abs_err_wr;

    struct ml_stats stats;
};

/* ---- integer-only API (models/model-xgb.c, no SSE) ---------------------------- */
void ml_init(struct ml_state *s);

/* Forgets the history (after a gap) but keeps the accuracy statistics. */
void ml_reset_history(struct ml_state *s);

static inline int ml_ready(const struct ml_state *s)
{
    return s->n_samples >= ML_HIST;
}

/*
 * Push one 1 ms sample (counts already normalised to 1 ms, in enum
 * ml_counter order). Integer only. At the end of each 10 ms window it scores
 * the pending prediction against the realised floor. Returns 1 when a
 * prediction is due now (window boundary and 20 samples available): the
 * caller should then call ml_predict().
 */
int ml_push(struct ml_state *s, const u32 cnt[ML_N_CNT]);

/*
 * Builds the features from the last 20 samples and predicts the floor of
 * the next 10 ms (MiB/s, clamped >= 0) inside ONE FPU section. Returns 1 on
 * success; 0 -- counted in stats.n_fpu_skipped, nothing floating-point
 * executed -- if the FPU cannot be used in the current context.
 */
int ml_predict(struct ml_state *s, s64 *pred_rd_mbps, s64 *pred_wr_mbps);

/* Arithmetic + model sanity check inside one FPU section. 0 = pass. */
int ml_selftest(void);

#endif /* MODEL_XGB_H */
