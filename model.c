#include "kernel_headers.h"
#include "ar.h"
#include "model.h"
#include "mlp_weights.h"    /* emlearn-exported weights (data only) */
#include "l2_bw_scalers.h"  /* MinMax constants from training */

/** Constants **/
#define H1 64
#define H2 32
#define MLP_IN (MLP_WINDOW * MLP_N_EV)

static_assert(L2_BW_N_FEATURES == MLP_N_EV, "scaler/feature count mismatch");
static_assert(L2_BW_WINDOW == MLP_WINDOW, "scaler/window size mismatch");

/* Model output is GB/s (1e9 B/s); the regulator works in MiB/s (see convert_events_to_mb) */
#define GBPS_TO_MIBPS (1e9f / (1024.0f * 1024.0f))

/* Per-core sliding window of normalised features. Once full, head is the oldest slot. */
struct mlp_state {
    float win[MLP_WINDOW][MLP_N_EV];
    u8 head;    /* next slot to write */
    u8 count;   /* valid slots, saturates at MLP_WINDOW */
};

static struct mlp_state state[MAX_NO_CPUS + 1];

/* Forward pass scratch. Only the master thread calls mlp_step(), so no locking. */
static float x[MLP_IN];
static float h1[H1];
static float h2[H2];


static inline float clamp01(float v){
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Weights are stored [input][output]: w[i * n_out + o] */
static float mlp_forward(const float *in){
    int o, i;
    float s;

    for (o = 0; o < H1; o++) {
        s = l2_bw_model_layer_0_biases[o];
        for (i = 0; i < MLP_IN; i++)
            s += l2_bw_model_layer_0_weights[i * H1 + o] * in[i];
        h1[o] = s > 0.0f ? s : 0.0f;
    }
    for (o = 0; o < H2; o++) {
        s = l2_bw_model_layer_1_biases[o];
        for (i = 0; i < H1; i++)
            s += l2_bw_model_layer_1_weights[i * H2 + o] * h1[i];
        h2[o] = s > 0.0f ? s : 0.0f;
    }
    s = l2_bw_model_layer_2_biases[0];
    for (i = 0; i < H2; i++)
        s += l2_bw_model_layer_2_weights[i] * h2[i];
    return s;
}

void mlp_reset(u8 cpu_id){
    /* All-zero bits is 0.0f, so no FP section needed */
    memset(&state[cpu_id], 0, sizeof(state[cpu_id]));
}

/* noinline: the caller brackets this with kernel_neon_begin()/end() from
 * non-FP code, so no FP instruction may be scheduled outside that section. */
noinline bool mlp_step(u8 cpu_id, const u64 delta[MLP_N_EV], const u64 running_ns[MLP_N_EV],
                       u64 *pred_mb){
    struct mlp_state *s = &state[cpu_id];
    bool ready;

    /* Counts per ns, then MinMax-scaled (clipped to [0,1]) with the training constants */
    for (int i = 0; i < MLP_N_EV; i++) {
        float cpns = running_ns[i] ? (float)delta[i] / (float)running_ns[i] : 0.0f;
        s->win[s->head][i] = clamp01((cpns - l2_bw_feat_min[i]) * l2_bw_feat_scale[i]);
    }
    s->head = (s->head + 1) % MLP_WINDOW;
    if (s->count < MLP_WINDOW)
        s->count++;

    ready = (s->count == MLP_WINDOW);
    if (ready) {
        /* Flatten oldest -> newest, matching feat[i : i+W].ravel() in training */
        for (int t = 0; t < MLP_WINDOW; t++)
            memcpy(&x[t * MLP_N_EV], s->win[(s->head + t) % MLP_WINDOW], sizeof(s->win[0]));

        float bw_mb = L2_BW_DENORM(mlp_forward(x)) * GBPS_TO_MIBPS;
        *pred_mb = (bw_mb > 0.0f) ? (u64)bw_mb : 0;
    }
    return ready;
}
