//
// Created by ss22 on 25/9/25.
//

#ifndef ADAPTIVEREGULATOR_MODEL_H
#define ADAPTIVEREGULATOR_MODEL_H


#define INITIAL_WEIGHT  0.1f

/* Weight matrix initialization (original function for read, new function for write) */
void initialize_weight_matrix(struct core_info *cinfo, bool first);      // Original - for read
void initialize_write_weight_matrix(struct core_info *cinfo, bool first); // NEW - for write

/* Weight matrix updates (original function for read, new function for write) */
void update_weight_matrix(s64 error, struct core_info *cinfo);           // Original - for read
void update_write_weight_matrix(s64 error, struct core_info *cinfo);     // NEW - for write

/* Bandwidth estimation function (used for both read and write) */
u64 estimate(u64* feat, u8 feat_len, double *wm, u8 wm_len, u8 index);

#endif //ADAPTIVEREGULATOR_MODEL_H
