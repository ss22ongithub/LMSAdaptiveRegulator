#include "kernel_headers.h"
#include "master.h"
#include "ar.h"
#include "ar_perfs.h"
#include "utils.h"
#include "model.h"

static struct task_struct* mthread = NULL;

/* Master thread state management */
static atomic_t master_state = ATOMIC_INIT(MASTER_STATE_INITIAL);
static wait_queue_head_t master_wait_queue;
/**/

/* External Functions */
extern void __throttle( void* cpu );
extern void __unthrottle( void* cpu );

/* External Variables / constants */
extern bool observe_only; /* Grant max budget always, never throttle */
extern ulong g_pool_bw_mb;
extern u8 g_num_regulated_cpus; /* CPU 0 is always master; set once in ar_init() */


/* Measures one interval of a regulated core, predicts its next-interval
 * bandwidth with the MLP and programs the read/write budgets accordingly. */
static void regulate_core(struct core_info *cinfo){
    u64 delta[MLP_N_EV], running[MLP_N_EV];
    u64 enabled, run, count;
    u64 pred_mb = 0;
    bool predicted;

    /* The events live on the regulated CPU, not on this (master) CPU.
     * perf_event_read_value() IPIs the owning CPU to fold in the live
     * hardware count before returning it. */
    for (u8 i = 0; i < MLP_N_EV; i++) {
        count = perf_event_read_value(cinfo->feat_event[i], &enabled, &run);
        delta[i] = count - cinfo->feat_count_prev[i];
        running[i] = run - cinfo->feat_running_prev[i];
        cinfo->feat_count_prev[i] = count;
        cinfo->feat_running_prev[i] = run;
    }

    /* First tick after (re)start only establishes the counter baselines */
    if (!cinfo->primed) {
        cinfo->primed = true;
        return;
    }

    /* Running totals for /sys/kernel/debug/ar/stats */
    stat_add(&cinfo->stats.ticks, 1);
    stat_add(&cinfo->stats.cycles, delta[MLP_EV_CYCLES]);
    stat_add(&cinfo->stats.instructions, delta[MLP_EV_INSTRUCTIONS]);
    stat_add(&cinfo->stats.l2_refill, delta[MLP_EV_L2D_REFILL]);
    stat_add(&cinfo->stats.l2_wb, delta[MLP_EV_L2D_WB]);

    cinfo->g_read_count_used = convert_events_to_mb(delta[MLP_EV_L2D_REFILL]);
    cinfo->g_write_count_used = convert_events_to_mb(delta[MLP_EV_L2D_WB]);
    u64 net_bandwidth_used = cinfo->g_read_count_used + cinfo->g_write_count_used;

    kernel_neon_begin();
    predicted = mlp_step(cinfo->cpu_id, delta, running, &pred_mb);
    kernel_neon_end();

    /* Until the 32-interval window fills, run on the initial budget. After
     * that initial_bw_mb is a floor so a near-zero prediction cannot starve
     * the core, and max_bw_limit_mb caps it. In observe_only mode the
     * estimate is still computed and logged, but the full cap is granted. */
    cinfo->next_estimate = predicted ? max(pred_mb, cinfo->initial_bw_mb) : cinfo->initial_bw_mb;
    u64 total_allocation = observe_only ? cinfo->max_bw_limit_mb :
                                          min(cinfo->next_estimate, cinfo->max_bw_limit_mb);

    /* Split by the read:write ratio of the interval just measured (50/50 when idle) */
    u64 read_allocation = net_bandwidth_used ?
        div64_u64(total_allocation * cinfo->g_read_count_used, net_bandwidth_used) :
        total_allocation / 2;
    u64 write_allocation = total_allocation - read_allocation;

    g_pool_bw_mb += cinfo->max_bw_limit_mb - total_allocation;

    /* Allocate budget separately to read and write counters.
     * perf_event_period() runs on the owning CPU: it stops the
     * event, installs the new period, clears period_left and
     * restarts it, so the next overflow fires after exactly this
     * many events. A period of 0 is rejected (-EINVAL), hence max 1. */
    perf_event_period(cinfo->read_event, max_t(u64, 1, convert_mb_to_events(read_allocation)));
    perf_event_period(cinfo->write_event, max_t(u64, 1, convert_mb_to_events(write_allocation)));

    /* Unthrottle if the core is in throttle state */
    atomic_set(&cinfo->throttler_task, false);

    AR_DEBUG("CPU(%u): r_bw_used=%llu w_bw_used=%llu net_bw_used=%llu prev_est=%llu err=%lld \
             pred=%llu%s est=%llu r_alloc=%llu w_alloc=%llu total_alloc=%llu Gpool=%lu\n",
             cinfo->cpu_id,
             cinfo->g_read_count_used,
             cinfo->g_write_count_used,
             net_bandwidth_used,
             cinfo->prev_estimate,
             (s64)net_bandwidth_used - (s64)cinfo->prev_estimate,
             pred_mb, predicted ? "" : "(warmup)",
             cinfo->next_estimate,
             read_allocation,
             write_allocation,
             total_allocation,
             g_pool_bw_mb);

    /* Save current estimate for next iteration */
    cinfo->prev_estimate = cinfo->next_estimate;
}


static int master_thread_func(void * data) {
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
    for (u8 cpu_id = 1; cpu_id <= g_num_regulated_cpus; cpu_id++) {
        struct core_info* cinfo = get_core_info(cpu_id);
        if (cinfo) {
            __unthrottle((void*)cinfo);
            pr_info("%s: CPU(%d) unthrottled", __func__, cpu_id);
        }
    }


    /* Step 4: Begin normal regulation loop */
    pr_info("%s: Starting normal regulation loop", __func__);

    ktime_t next_tick = ktime_get();

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
            next_tick = ktime_get();
        }

        if (kthread_should_stop()){
            pr_info("Stopping thread %s\n",__func__);
            break;
        }

        for (u8 cpu_id = 1; cpu_id <= g_num_regulated_cpus; cpu_id++) {
            struct core_info* cinfo = get_core_info(cpu_id);
            if (WARN_ON(cinfo == NULL) || !cpu_online(cpu_id))
                continue;
            regulate_core(cinfo);
        }

        /* Sleep to an absolute hrtimer deadline so the period stays at the
         * regulation interval (2.27 ms, the model's training cadence)
         * regardless of how long the loop above took. After an overrun,
         * skip the missed ticks rather than running them back to back. */
        ktime_t now = ktime_get();
        next_tick = ktime_add_us(next_tick, get_regulation_time());
        if (ktime_before(next_tick, now))
            next_tick = ktime_add_us(now, get_regulation_time());
        set_current_state(TASK_INTERRUPTIBLE);
        schedule_hrtimeout_range(&next_tick, 0, HRTIMER_MODE_ABS);
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
