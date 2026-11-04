/*
 * models/model-xgb-fpu.h -- the FLOATING-POINT half of the ML predictor (implemented in
 * models/model-xgb-fpu.c).
 *
 * RULES (they are what keeps the kernel safe):
 *
 *  1. models/model-xgb-fpu.c is the ONLY object in the module compiled with SSE/double
 *     enabled (see Makefile: CFLAGS_ml_fpu.o). Every other object keeps the
 *     kernel's default -mno-sse -mno-sse2 -mno-mmx -mno-80387, so the
 *     compiler cannot emit a single FP/SSE instruction there.
 *  2. Every function in models/model-xgb-fpu.c may only run between kernel_fpu_begin() and
 *     kernel_fpu_end(). Those two calls exist in exactly one place,
 *     models/model-xgb.c, which is built WITHOUT SSE, and each pair brackets exactly
 *     one call into this file. Nothing here sleeps, allocates, or calls back
 *     into the kernel.
 *  3. The entry points below take only integers and pointers, because a
 *     caller built without SSE cannot pass a double/float.
 *
 * host/fpu_containment_check.py verifies 1 and 2 mechanically.
 */
#ifndef MODEL_XGB_FPU_H
#define MODEL_XGB_FPU_H

#include "model-xgb.h"

/* ---- entry points for models/model-xgb.c (integer-only signatures) --------------- */

/*
 * Features from the last ML_HIST samples of the ring (requires ml_ready())
 * -> predicted floor of the next 10 ms, MiB/s, clamped to [0, ML_PRED_MAX].
 */
void ml_predict_fpu(const struct ml_state *s, s64 *pred_rd_mbps, s64 *pred_wr_mbps);

/* Arithmetic + model sanity checks. Returns 0 if all pass, else a code
 * identifying the failed check. */
int ml_selftest_fpu(void);

#define ML_PRED_MAX_MBPS    1000000     /* far above any real bandwidth */

/* ---- internals, visible only to models/model-xgb-fpu.c and the host tests ------------ */
#ifdef ML_FPU_INTERNAL

/* Builds the model features from the ring (requires ml_ready()). */
void ml_build_features(const struct ml_state *s, float out[ML_N_FEATURES]);

/* Raw, unclamped forest outputs (MiB/s) for given features; the host replay
 * compares them with Python. */
void ml_forest(const float f[ML_N_FEATURES], float *rd, float *wr);

/* forest output -> integer MiB/s: NaN / negative -> 0, capped, truncated */
s64 ml_pred_to_mbps(float p);

#endif /* ML_FPU_INTERNAL */

#endif /* MODEL_XGB_FPU_H */
