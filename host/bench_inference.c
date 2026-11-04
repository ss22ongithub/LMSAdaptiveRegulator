/*
 * bench_inference.c -- rough cost of one prediction (feature build + both
 * forests = what runs inside ONE kernel FPU section) on THIS machine.
 * Synthetic samples in realistic ranges; branch behaviour of the trees
 * depends on the data, so treat the result as an order of magnitude, not a
 * kernel measurement (different CPU, 32-bit host compiler). The kernel
 * reports the real numbers in debugfs ml_stats (infer_us, infer_max_us) and
 * the probe module prints the slowest ml_predict().
 *
 * Built by verify_replay.py (host/build/bench_inference.exe).
 */
#include <stdio.h>
#include <time.h>

#define ML_FPU_INTERNAL
#include "../models/model-xgb.h"
#include "../models/model-xgb-fpu.h"

static struct ml_state s;
static unsigned long long rng = 88172645463325252ULL;

static unsigned long long rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

int main(void)
{
    const int n = 20000;
    s64 pr = 0, pw = 0, sink = 0;
    clock_t t0, t1;
    int i, c, preds = 0;

    ml_init(&s);
    t0 = clock();
    for (i = 0; i < n * ML_WINDOW; i++) {
        u32 cnt[ML_N_CNT];

        for (c = 0; c < ML_N_CNT; c++)
            cnt[c] = (u32)(rnd() % 400000);
        cnt[ML_CYC] = 1000000 + (u32)(rnd() % 3000000);
        cnt[ML_INS] = (u32)(rnd() % 9000000);
        if (ml_push(&s, cnt)) {
            ml_predict_fpu(&s, &pr, &pw);
            sink += pr + pw;
            preds++;
        }
    }
    t1 = clock();
    printf("%d predictions: %.2f us per prediction incl. 10 integer pushes (sink %lld)\n", preds,
           (double)(t1 - t0) * 1e6 / CLOCKS_PER_SEC / preds, (long long)sink);
    return 0;
}
