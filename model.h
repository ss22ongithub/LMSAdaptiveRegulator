//
// Created by ss22 on 25/9/25.
//

#ifndef ADAPTIVEREGULATOR_MODEL_H
#define ADAPTIVEREGULATOR_MODEL_H

/*
 * MLP bandwidth predictor (224 -> 64 -> 32 -> 1, ReLU) trained in PyTorch and
 * exported through emlearn (mlp_weights.h, l2_bw_scalers.h).
 *
 * model.c is the only object built with floating point enabled. This
 * interface is integer-only on purpose, so no FP state leaks into the rest
 * of the module.
 */

/* Feature order must match EVENTS in mlp_bw_predictor.py */
enum mlp_feature {
    MLP_EV_CYCLES,
    MLP_EV_INSTRUCTIONS,
    MLP_EV_L1I_CACHE,
    MLP_EV_L2D_REFILL,
    MLP_EV_L2D_WB,
    MLP_EV_BUS_ACCESS,
    MLP_EV_BUS_CYCLES,
    MLP_N_EV
};

/* Look-back window (number of regulation intervals) the model was trained on */
#define MLP_WINDOW 32

/* Clear the sliding window of a core (call before (re)starting regulation) */
void mlp_reset(u8 cpu_id);

/*
 * Call only between kernel_neon_begin() and kernel_neon_end().
 * Push one regulation interval of counter deltas for a core and predict the
 * L2 bandwidth (refill + write-back) of the next interval.
 * @delta:      event count increments over the interval, in enum mlp_feature order
 * @running_ns: time each event was actually counting over the interval
 * @pred_mb:    predicted bandwidth in MiB/s, written only when returning true
 *
 * Returns false while the window is still warming up (< MLP_WINDOW intervals).
 */
bool mlp_step(u8 cpu_id, const u64 delta[MLP_N_EV], const u64 running_ns[MLP_N_EV],
              u64 *pred_mb);

#endif //ADAPTIVEREGULATOR_MODEL_H
