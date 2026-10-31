/* Auto-generated MinMaxScaler params for l2_bw (window=32) */
/* Feature scaler: 7 values (one per feature) */

#pragma once

#define L2_BW_N_FEATURES   7
#define L2_BW_WINDOW        32
#define L2_BW_INPUT_DIM     224  /* WINDOW * N_FEATURES */

/* feat_min[i]: minimum value of feature i (from training) */
static const float l2_bw_feat_min[L2_BW_N_FEATURES] = {
    4.39214855e-01f,
    5.96657358e-02f,
    2.05477122e-02f,
    2.25308177e-04f,
    1.20980701e-04f,
    1.67854596e-03f,
    1.47046015e-01f
};

/* feat_scale[i] = 1/(max-min) for feature i */
static const float l2_bw_feat_scale[L2_BW_N_FEATURES] = {
    7.53076971e-01f,
    3.41209561e-01f,
    9.79275405e-01f,
    1.58547649e+01f,
    4.67244568e+01f,
    3.96353126e+00f,
    1.75852251e+00f
};

/* Target inverse MinMax: y_real = y_norm * tgt_range + tgt_min */
static const float l2_bw_tgt_min   = 2.05104681e-02f;
static const float l2_bw_tgt_range = 8.41880051e+00f;

#define L2_BW_DENORM(y_norm) \
    ((float)(y_norm) * l2_bw_tgt_range + l2_bw_tgt_min)

/* Normalise a single feature value: */
/* norm = (raw - feat_min[i]) * feat_scale[i]  (clipped to [0,1]) */
#define L2_BW_NORM_FEAT(raw, i) \
    fmaxf(0.0f, fminf(1.0f, ((float)(raw) - l2_bw_feat_min[i]) * l2_bw_feat_scale[i]))
