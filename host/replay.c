/*
 * replay.c -- "live" replay of one run through models/model-xgb.c / models/model-xgb-fpu.c.
 *
 * Feeds the run to the predictor ONE 1 ms SAMPLE AT A TIME, exactly as the
 * kernel master thread does: ml_push() every sample; when it reports that a
 * prediction is due (every 10th sample once 20 consecutive samples exist),
 * the features and the forest outputs are computed. Nothing is looked ahead
 * or computed in bulk.
 *
 *   replay <samples.csv> <predictions.csv>
 *
 * samples.csv (written by verify_replay.py from bw1ms_features.py):
 *   win,reset,<one column per model counter, enum ml_counter order>
 *   integer counts per 1 ms; reset=1 marks the first sample after samples
 *   the training data drops (the kernel restarts its history there too).
 * predictions.csv:
 *   win,f0..f{N-1},raw_rd,raw_wr,int_rd,int_wr
 *   win = window index of the newest sample, features and raw forest
 *   outputs as float32 (9 significant digits = exact), int_* = the integer
 *   MiB/s the kernel would use (ml_predict_fpu()).
 * stderr: one "STATS ..." line with the online-accuracy counters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ML_FPU_INTERNAL
#include "../models/model-xgb.h"
#include "../models/model-xgb-fpu.h"

#define LINE_BYTES 4096

static struct ml_state state;

int main(int argc, char **argv)
{
    static char line[LINE_BYTES];
    FILE *in, *out;
    long rows = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <samples.csv> <predictions.csv>\n", argv[0]);
        return 2;
    }
    in = fopen(argv[1], "r");
    out = fopen(argv[2], "w");
    if (in == NULL || out == NULL) {
        fprintf(stderr, "cannot open files\n");
        return 2;
    }
    if (fgets(line, sizeof(line), in) == NULL) {
        fprintf(stderr, "empty file\n");
        return 2;
    }

    ml_init(&state);
    fputs("win", out);
    for (int i = 0; i < ML_N_FEATURES; i++)
        fprintf(out, ",f%d", i);
    fputs(",raw_rd,raw_wr,int_rd,int_wr\n", out);

    while (fgets(line, sizeof(line), in) != NULL) {
        u32 cnt[ML_N_CNT];
        long win, reset;
        char *p = line, *end;

        win = strtol(p, &end, 10);
        if (*end != ',') goto bad;
        p = end + 1;
        reset = strtol(p, &end, 10);
        if (*end != ',') goto bad;
        p = end + 1;
        for (int c = 0; c < ML_N_CNT; c++) {
            unsigned long v = strtoul(p, &end, 10);

            if (end == p || (c < ML_N_CNT - 1 && *end != ',')) goto bad;
            cnt[c] = (u32)v;
            p = end + 1;
        }
        rows++;

        if (reset)
            ml_reset_history(&state);
        if (ml_push(&state, cnt)) {
            float f[ML_N_FEATURES], pr, pw;
            s64 ir, iw;

            /* features + raw outputs for the comparison with Python ... */
            ml_build_features(&state, f);
            ml_forest(f, &pr, &pw);
            /* ... and the real entry point the kernel calls (on the host its
             * FPU macros are no-ops), which also does the bookkeeping */
            if (!ml_predict(&state, &ir, &iw)) {
                fprintf(stderr, "ml_predict returned 0\n");
                return 1;
            }

            fprintf(out, "%ld", win);
            for (int i = 0; i < ML_N_FEATURES; i++)
                fprintf(out, ",%.9g", (double)f[i]);
            fprintf(out, ",%.9g,%.9g,%lld,%lld\n", (double)pr, (double)pw,
                    (long long)ir, (long long)iw);
        }
        continue;
bad:
        fprintf(stderr, "bad input row %ld\n", rows + 1);
        return 1;
    }

    fprintf(stderr,
            "STATS rows=%ld n_pred=%u n_eval=%u abs_rd=%llu abs_wr=%llu over_rd=%llu over_wr=%llu resets=%u\n",
            rows, state.stats.n_pred, state.stats.n_eval,
            (unsigned long long)state.abs_err_rd, (unsigned long long)state.abs_err_wr,
            (unsigned long long)state.n_over_rd, (unsigned long long)state.n_over_wr,
            state.stats.n_resets);
    fclose(in);
    fclose(out);
    return 0;
}
