#include "kernel_headers.h"
#include "master.h"
#include "ar.h"
#include "ar_perfs.h"
#include "utils.h"
#include "models/model-xgb.h"

static struct task_struct* mthread = NULL;

/* Master thread state management */
static atomic_t master_state = ATOMIC_INIT(MASTER_STATE_INITIAL);
static wait_queue_head_t master_wait_queue;
/**/

/* External Functions */
extern void __throttle( void* cpu );
extern void __unthrottle( void* cpu );

/* External Variables / constants */
extern u64 g_initial_bw_mb[MAX_NO_CPUS+1];/*Pre-defined initial / min Bandwidth in MB/s */
extern u64 g_percore_bw_limit_mb[MAX_NO_CPUS+1]; /*Per-core max Bandwidth in MB/s (from ml_max_bw_mb) */
extern ulong g_pool_bw_mb;
extern u8 g_num_regulated_cpus; /* CPU 0 is always master; set once in ar_init() */

/**************************************************************************
 * Per-core counter sampling and budget re-arming -- ON THE OWNING CPU
 *
 * The original loop called pmu->stop()/pmu->start() on CPU 1..4's events
 * from the master thread on CPU 0. On x86 those callbacks act on the
 * *calling* CPU's PMU (they read the counter with rdpmc and touch the
 * per-CPU active mask), so the counts and the re-arm could hit CPU 0's
 * counters instead. Here they run on the CPU that owns the events, through
 * smp_call_function_single(), which is the MemGuard pattern (stop with
 * PERF_EF_UPDATE, set period_left, start with PERF_EF_RELOAD, all on the
 * owning CPU with interrupts off).
 *
 * ar_sample_and_rearm() runs in hard-IRQ context on the target CPU:
 *   - no sleeping, no locks the master holds, NO floating point;
 *   - every event is touched only if it is ACTIVE on this very CPU
 *     (a disabled event, or one perf could not schedule, is left alone);
 *   - it re-arms the throttling events with the budget the master computed
 *     in the previous iteration (arm_rd_events / arm_wr_events).
 **************************************************************************/

static inline bool ar_event_active_here(struct perf_event *e)
{
    return e && e->state == PERF_EVENT_STATE_ACTIVE && e->oncpu == (int)smp_processor_id();
}

static void ar_sample_and_rearm(void *info)
{
    struct core_info *c = info;
    struct perf_event *rd = c->read_event, *wr = c->write_event;
    const bool rd_on = ar_event_active_here(rd);
    const bool wr_on = ar_event_active_here(wr);
    int i;

    if (rd_on)
        rd->pmu->stop(rd, PERF_EF_UPDATE);
    if (wr_on)
        wr->pmu->stop(wr, PERF_EF_UPDATE);

    /* Plain counting events: refresh event->count from the hardware. */
    for (i = 0; i < ML_N_CNT; i++) {
        struct perf_event *e = c->cnt_event[i];

        if (i != ML_RD && i != ML_WR && ar_event_active_here(e))
            e->pmu->read(e);
    }

    c->snap_ns = ktime_get_ns();
    for (i = 0; i < ML_N_CNT; i++) {
        struct perf_event *e = (i == ML_RD) ? rd : (i == ML_WR) ? wr : c->cnt_event[i];

        c->snap[i] = e ? perf_event_count(e) : 0;
    }
    c->snap_all_active = rd_on && wr_on;
    for (i = 0; i < ML_N_CNT; i++)
        if (i != ML_RD && i != ML_WR && !ar_event_active_here(c->cnt_event[i]))
            c->snap_all_active = false;

    /* Re-arm the budget for the next period and restart. */
    if (rd_on) {
        local64_set(&rd->hw.period_left, c->arm_rd_events);
        rd->pmu->start(rd, PERF_EF_RELOAD);
    }
    if (wr_on) {
        local64_set(&wr->hw.period_left, c->arm_wr_events);
        wr->pmu->start(wr, PERF_EF_RELOAD);
    }
}

/**************************************************************************
 * ML bandwidth predictor (replaces the LMS estimator of the original)
 *
 * Model: xgboost_mem_next_1ms (simpleML_memguard/xgboost_mem_1ms.py). Every
 * loop iteration (1 ms) per regulated core:
 *   1. the counter deltas since the previous sample are normalised to
 *      exactly 1 ms (integer) and pushed into the history ring
 *      (ml_push(): integer only, no FPU);
 *   2. every 10th sample, once 20 consecutive samples exist, ml_predict()
 *      predicts the floor of the per-core read / write bandwidth over the
 *      NEXT 10 ms -- ONE kernel_fpu_begin()/end() section per core per 10 ms;
 *   3. the prediction becomes the per-class budget (MiB/s).
 * All counter values come from ar_sample_and_rearm() above; nothing here
 * sleeps or reads a counter.
 **************************************************************************/

/* delta * 1 ms / elapsed, without 64-bit overflow, saturated to u32 */
static inline u32 ml_normalise(u64 delta, u64 elapsed_ns)
{
    u64 v = mul_u64_u64_div_u64(delta, ML_SAMPLE_NS, elapsed_ns);

    return v > 0xffffffffULL ? 0xffffffffU : (u32)v;
}

/* prediction (MiB/s) -> budget for one traffic class (MiB/s) */
static s64 ml_budget_mb(const struct core_info *cinfo, s64 pred_mbps)
{
    const s64 scale = min_t(ulong, ml_scale_pct, 1000);
    s64 v = div_s64(pred_mbps * scale, 100) + (s64)ml_headroom_mb;

    return clamp_t(s64, v, (s64)cinfo->initial_bw_mb, (s64)cinfo->max_bw_limit_mb);
}

/* Takes the current snapshot as the start of the next sample. */
static void ml_prime(struct core_info *cinfo)
{
    int i;

    for (i = 0; i < ML_N_CNT; i++)
        cinfo->ml_prev[i] = cinfo->snap[i];
    cinfo->ml_prev_ns = cinfo->snap_ns;
    cinfo->ml_primed = true;
}

/* Clears the predictor and restarts from the warm-up budget. */
static void ml_restart(struct core_info *cinfo)
{
    ml_init(&cinfo->ml);
    cinfo->ml_primed = false;
    cinfo->next_estimate = cinfo->initial_bw_mb;
    cinfo->prev_estimate = cinfo->initial_bw_mb;
    cinfo->write_next_estimate = cinfo->initial_bw_mb;
    cinfo->write_prev_estimate = cinfo->initial_bw_mb;
}

static void ml_restart_all(void)
{
    for (u8 cpu_id = 1; cpu_id <= g_num_regulated_cpus; cpu_id++) {
        struct core_info *cinfo = get_core_info(cpu_id);

        if (cinfo)
            ml_restart(cinfo);
    }
}

/*
 * ML kill switch (see ml_enable in ar.c). Only the master thread touches
 * ml_active and ml_selftest_state. While ml_active is false no floating-point
 * code runs and every core keeps the warm-up budget.
 */
int ml_selftest_state;          /* 0 = not run yet, 1 = passed, -1 = failed */
static bool ml_active;

static void ml_sync_enable(void)
{
    bool want = READ_ONCE(ml_enable);

    if (want == ml_active)
        return;

    if (want && ml_selftest_state == 0) {
        /* Process context, no locks held, not inside any FPU section. */
        int rc = ml_selftest();

        ml_selftest_state = rc ? -1 : 1;
        if (rc)
            pr_err("ML self-test FAILED (code %d): predictor stays OFF, budgets stay at "
                   "the warm-up value. Reload the module to retry.\n", rc);
        else
            pr_info("ML self-test passed\n");
    }
    if (want && !ar_ml_counters_available()) {
        pr_err("ML predictor needs every model counter on every regulated core "
               "(see the 'ML counter ... unavailable' messages above): staying OFF\n");
        WRITE_ONCE(ml_enable, false);
        want = false;
    }
    if (want && ml_selftest_state != 1) {
        WRITE_ONCE(ml_enable, false);   /* never leave the switch on while refusing to run */
        want = false;
    }

    ml_active = want;
    ml_restart_all();                   /* fresh history + warm-up budget on every switch */
    pr_info("ML predictor %s\n", want ? "ENABLED" : "disabled");
}

/*
 * Called once per loop iteration, right after ar_sample_and_rearm() took a
 * fresh snapshot of this core's counters. Between predictions the cached
 * next_estimate / write_next_estimate (MiB/s) are left untouched.
 */
static void ml_update_budget(struct core_info *cinfo)
{
    u32 cnt[ML_N_CNT];
    u64 dt, t0;
    s64 pred_rd = 0, pred_wr = 0;
    int i;

    if (!cinfo->ml_primed) {
        ml_prime(cinfo);
        return;
    }

    dt = cinfo->snap_ns - cinfo->ml_prev_ns;

    /* Shorter than a sample (the loop woke early): keep accumulating. */
    if (dt < ML_DT_MIN_NS)
        return;

    /* A gap (thread starved, regulation paused) or a counter that is not
     * running breaks the "consecutive 1 ms samples" meaning of the history:
     * start the history over, but keep the budget currently in force. The
     * training data drops such samples the same way (bw1ms_features.py). */
    if (dt > ML_DT_MAX_NS || !cinfo->snap_all_active) {
        ml_reset_history(&cinfo->ml);  /* counted in ml.stats.n_resets */
        ml_prime(cinfo);
        return;
    }

    for (i = 0; i < ML_N_CNT; i++)
        cnt[i] = ml_normalise(cinfo->snap[i] - cinfo->ml_prev[i], dt);
    ml_prime(cinfo);

    if (!ml_push(&cinfo->ml, cnt))
        return;                         /* no prediction due this sample */

    t0 = ktime_get_ns();
    if (ml_predict(&cinfo->ml, &pred_rd, &pred_wr)) {
        cinfo->next_estimate = ml_budget_mb(cinfo, pred_rd);
        cinfo->write_next_estimate = ml_budget_mb(cinfo, pred_wr);
    }
    cinfo->ml_infer_ns = (u32)(ktime_get_ns() - t0);
    if (cinfo->ml_infer_ns > cinfo->ml_infer_max_ns)
        cinfo->ml_infer_max_ns = cinfo->ml_infer_ns;
}

/* Budget armed before the first prediction, and after every restart. */
static void ar_arm_initial(struct core_info *cinfo)
{
    cinfo->arm_rd_events = convert_mb_to_events(cinfo->initial_bw_mb);
    cinfo->arm_wr_events = convert_mb_to_events(cinfo->initial_bw_mb);
}

/* One regulation period for one core. */
static void ar_regulate_core(struct core_info *cinfo)
{
    s64 total_next_estimate, read_error __maybe_unused, write_error __maybe_unused;
    u64 read_allocation, write_allocation, total_allocation;

    /* 1. snapshot + re-arm, on the core itself; waits for it to finish */
    if (smp_call_function_single(cinfo->cpu_id, ar_sample_and_rearm, cinfo, 1) != 0) {
        cinfo->ipi_failures++;
        return;
    }

    /* Read bandwidth tracking */
    cinfo->g_read_count_old = cinfo->g_read_count_new;
    cinfo->g_read_count_new = convert_events_to_mb(cinfo->snap[ML_RD]);
    cinfo->g_read_count_used = cinfo->g_read_count_new - cinfo->g_read_count_old;

    /* Write bandwidth tracking */
    cinfo->g_write_count_old = cinfo->g_write_count_new;
    cinfo->g_write_count_new = convert_events_to_mb(cinfo->snap[ML_WR]);
    cinfo->g_write_count_used = cinfo->g_write_count_new - cinfo->g_write_count_old;

    /* 2. ML replaces the LMS estimate: refresh the read / write budget
     *    estimates (MiB/s) every 10 ms; otherwise next_estimate /
     *    write_next_estimate keep the last prediction (or the warm-up
     *    budget). Skipped entirely while the ML switch is off. */
    if (ml_active)
        ml_update_budget(cinfo);

    /* 3. Separate allocation for read and write based on estimates */
    total_next_estimate = cinfo->next_estimate + cinfo->write_next_estimate;

    if (total_next_estimate <= (s64)cinfo->max_bw_limit_mb) {
        /* Total estimate within limit - allocate as estimated */
        read_allocation = cinfo->next_estimate;
        write_allocation = cinfo->write_next_estimate;
    } else {
        /* Total estimate exceeds limit - allocate proportionally */
        read_allocation = (cinfo->max_bw_limit_mb * cinfo->next_estimate) / total_next_estimate;
        write_allocation = (cinfo->max_bw_limit_mb * cinfo->write_next_estimate) / total_next_estimate;
    }

    total_allocation = read_allocation + write_allocation;
    /* (was max(0, max - total) on u64s, which wraps to a huge pool if the
     * allocation ever exceeds the cap) */
    if (cinfo->max_bw_limit_mb > total_allocation)
        g_pool_bw_mb += cinfo->max_bw_limit_mb - total_allocation;

    /* Armed on the core by the next ar_sample_and_rearm() (1 ms later). */
    cinfo->arm_rd_events = convert_mb_to_events(read_allocation);
    cinfo->arm_wr_events = convert_mb_to_events(write_allocation);

    /* Unthrottle if the core is in throttle state (its counters were just
     * re-armed with a fresh period budget) */
    atomic_set(&cinfo->throttler_task, false);

    /* Budget vs. what was actually used in the last loop period */
    read_error = cinfo->g_read_count_used - cinfo->prev_estimate;
    write_error = cinfo->g_write_count_used - cinfo->write_prev_estimate;

    AR_DEBUG("CPU(%u):r_bw_used=%llu r_est=%lld r_alloc=%llu w_bw_used=%llu w_est=%lld w_alloc=%llu \
    total_est=%lld total_alloc=%llu r_err=%lld w_err=%lld Gpool=%lu \
    ml_ready=%d ml_pred_r=%lld ml_pred_w=%lld ml_infer_ns=%u\n",
                 cinfo->cpu_id,
                 cinfo->g_read_count_used,
                 cinfo->next_estimate,
                 read_allocation,
                 cinfo->g_write_count_used,
                 cinfo->write_next_estimate,
                 write_allocation,
                 total_next_estimate,
                 total_allocation,
                 read_error,
                 write_error,
                 g_pool_bw_mb,
                 ml_ready(&cinfo->ml),
                 cinfo->ml.stats.pred_rd_mbps,
                 cinfo->ml.stats.pred_wr_mbps,
                 cinfo->ml_infer_ns);

    /* Save current estimates for next iteration */
    cinfo->prev_estimate = cinfo->next_estimate;
    cinfo->write_prev_estimate = cinfo->write_next_estimate;
}

/* Restart of regulation: fresh ML history, warm-up budget armed. */
static void ar_restart_all(void)
{
    ml_restart_all();
    for (u8 cpu_id = 1; cpu_id <= g_num_regulated_cpus; cpu_id++) {
        struct core_info *cinfo = get_core_info(cpu_id);

        if (cinfo)
            ar_arm_initial(cinfo);
    }
}

/*
 * Sleeps until the next 1 ms period boundary. The original msleep(1) sleeps
 * at least one jiffy more than asked (about 2 ms at HZ=1000, 4-8 ms at
 * HZ=250); the model is trained on 1 ms samples, so the loop now keeps an
 * absolute 1 ms schedule with high-resolution sleeps. If an iteration
 * overran, the schedule restarts from now instead of trying to catch up.
 */
static void ar_sleep_until_next_period(u64 *next_ns)
{
    u64 now = ktime_get_ns();

    *next_ns += ML_SAMPLE_NS;
    if (*next_ns <= now) {
        *next_ns = now + ML_SAMPLE_NS;
    }
    {
        unsigned long us = (unsigned long)div64_u64(*next_ns - now, 1000ULL);

        usleep_range(us, us + 20);
    }
}

static int master_thread_func(void * data) {
    u64 next_period_ns;

    pr_info("%s: Enter",__func__);

    /* Step 1: Immediately throttle all regulated cores */
    pr_info("%s: Throttling all cores at startup", __func__);
    for (u8 cpu_id = 1; cpu_id <= g_num_regulated_cpus; cpu_id++) {
        struct core_info* cinfo = get_core_info(cpu_id);
        if (cinfo) {
            __throttle((void*)cinfo);
            pr_info("%s: CPU(%d) throttled", __func__, cpu_id);
        }
    }

    /* Step 2: Wait in INITIAL state until regulation is enabled */
    pr_info("%s: Entering INITIAL state, waiting for regulation to start", __func__);
    wait_event_interruptible(master_wait_queue,
                             atomic_read(&master_state) == MASTER_STATE_RUNNING
                             || kthread_should_stop());

    if (kthread_should_stop()) {
	    pr_info("%s: Exit",__func__);
        return 0;
    }

    /* Step 3: Unthrottle all cores when regulation begins */
    pr_info("%s: Regulation enabled, unthrottling all cores", __func__);
    ar_restart_all();   /* fresh history, warm-up budget */
    for (u8 cpu_id = 1; cpu_id <= g_num_regulated_cpus; cpu_id++) {
        struct core_info* cinfo = get_core_info(cpu_id);
        if (cinfo) {
            __unthrottle((void*)cinfo);
            pr_info("%s: CPU(%d) unthrottled", __func__, cpu_id);
        }
    }


    /* Step 4: Begin normal regulation loop */
    pr_info("%s: Starting normal regulation loop", __func__);
    next_period_ns = ktime_get_ns();

    while (!kthread_should_stop() ) {
        /* Check if we should pause regulation */
        if (atomic_read(&master_state) != MASTER_STATE_RUNNING) {
            pr_info("%s: Regulation paused, waiting...", __func__);
            wait_event_interruptible(master_wait_queue,
                                     atomic_read(&master_state) == MASTER_STATE_RUNNING
                                     || kthread_should_stop());
            if (kthread_should_stop()) {
                break;
            }
            pr_info("%s: Regulation resumed", __func__);
            ar_restart_all();   /* the paused time is not part of any window */
            next_period_ns = ktime_get_ns();
        }

        if (kthread_should_stop()){
            pr_info("Stopping thread %s\n",__func__);
            break;
        }

        ml_sync_enable();       /* picks up changes of the ml_enable switch */

        for (u8 cpu_id = 1; cpu_id <= g_num_regulated_cpus; cpu_id++) {
            struct core_info *cinfo = get_core_info(cpu_id);

            if (cinfo == NULL || !cpu_online(cpu_id))
                continue;
            WARN_ON(cinfo->read_event == NULL);
            WARN_ON(cinfo->write_event == NULL);
            if (cinfo->read_event == NULL || cinfo->write_event == NULL)
                continue;
            ar_regulate_core(cinfo);
        }

        ar_sleep_until_next_period(&next_period_ns);
    }

    pr_info("%s: Exit",__func__);
    return 0;
}


void initialize_master(void){

    const u8 cpu_id_zero  = 0; //cpuid= 1,2,3 4 are reserved for BW regulation

    /* Initialize wait queue for master thread state changes */
    init_waitqueue_head(&master_wait_queue);

    /* Set initial state */
    atomic_set(&master_state, MASTER_STATE_INITIAL);

    mthread = kthread_create_on_node(master_thread_func,
                                       (void*)NULL,
                                       cpu_to_node(cpu_id_zero),
                                       "areg_master_thread/%d",cpu_id_zero);
    BUG_ON(IS_ERR(mthread));
    kthread_bind(mthread, cpu_id_zero);
    wake_up_process(mthread);

    pr_info("%s: Master thread initialized in INITIAL state", __func__);
}

void deinitialize_master(void){
    if (mthread){
        /* Wake up master thread if it's waiting */
        atomic_set(&master_state, MASTER_STATE_STOPPED);
        wake_up_interruptible(&master_wait_queue);

        kthread_stop(mthread);
        mthread = NULL;
    }
    pr_info("%s: Exit!",__func__ );
}

void master_start_regulation(void){
    pr_info("%s: Starting regulation", __func__);
    atomic_set(&master_state, MASTER_STATE_RUNNING);
    wake_up_interruptible(&master_wait_queue);
}

void master_stop_regulation(void){
    pr_info("%s: Stopping regulation", __func__);
    atomic_set(&master_state, MASTER_STATE_INITIAL);
}

int master_get_state(void){
    return atomic_read(&master_state);
}
