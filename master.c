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
extern u64 estimate(u64* feat, u8 feat_len, double *wm, u8 wm_len, u8 index);
extern void update_weight_matrix(s64 error, struct core_info* cinfo);
extern void update_write_weight_matrix(s64 error, struct core_info* cinfo);

extern void __throttle( void* cpu );
extern void __unthrottle( void* cpu );

/* External Variables / constants */
extern u64 g_initial_bw_mb[MAX_NO_CPUS+1];/*Pre-defined initial / min Bandwidth in MB/s */
extern const u64 g_percore_bw_limit_mb[MAX_NO_CPUS+1]; /*Pre-defined max Bandwidth per core in MB/s */
extern ulong g_pool_bw_mb;
extern u8 g_num_regulated_cpus; /* CPU 0 is always master; set once in ar_init() */


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


    while (!kthread_should_stop() ) {
        u8 cpu_id;
        
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
        }

        if (kthread_should_stop()){
            pr_info("Stopping thread %s\n",__func__);
            break;
        }

        for_each_online_cpu(cpu_id){
            s64 bw_total_req = 0;
            switch(cpu_id){
                case 1:
                case 2:
                case 3:
                case 4:
                    struct core_info* cinfo = get_core_info(cpu_id);
                    WARN_ON(cinfo == NULL);
                    WARN_ON(cinfo->read_event == NULL);
                    WARN_ON(cinfo->write_event == NULL);

                    struct perf_event* read_event = cinfo->read_event;
                    struct perf_event* write_event = cinfo->write_event;
                    //struct perf_event* cycles_l3miss_event = cinfo->cycles_l3miss_event;

                    /* Stop the counters */
                    cinfo->read_event->pmu->stop(cinfo->read_event, PERF_EF_UPDATE);
                    cinfo->write_event->pmu->stop(cinfo->write_event, PERF_EF_UPDATE);
                    //cinfo->cycles_l3miss_event->pmu->stop(cinfo->cycles_l3miss_event,PERF_EF_UPDATE);

                    /* Read bandwidth tracking */
                    cinfo->g_read_count_old = cinfo->g_read_count_new;
                    cinfo->g_read_count_new = convert_events_to_mb(perf_event_count(read_event));
                    cinfo->g_read_count_used = cinfo->g_read_count_new - cinfo->g_read_count_old;

                    /* Write bandwidth tracking */
                    cinfo->g_write_count_old = cinfo->g_write_count_new;
                    cinfo->g_write_count_new = convert_events_to_mb(perf_event_count(write_event));
                    cinfo->g_write_count_used = cinfo->g_write_count_new - cinfo->g_write_count_old;

                    /* Net bandwidth = read + write */
                    u64 net_bandwidth_used = cinfo->g_read_count_used + cinfo->g_write_count_used;

                    //u64 cycles_l3miss_count = perf_event_count(cycles_l3miss_event);

                    bw_total_req += net_bandwidth_used;

                    /* Update read bandwidth history and estimate */
                    cinfo->read_event_hist[cinfo->ri] = cinfo->g_read_count_used;
                    cinfo->next_estimate = estimate(
                        cinfo->read_event_hist,
                        sizeof(cinfo->read_event_hist)/sizeof(cinfo->read_event_hist[0]),
                        cinfo->weight_matrix,
                        sizeof(cinfo->weight_matrix)/sizeof(cinfo->weight_matrix[0]),
                        cinfo->ri
                    ) + cinfo->initial_bw_mb;

                    /* Check for negative read estimate */
                    if(cinfo->next_estimate < 0){
                        AR_DEBUG("CPU(%u): Negative Read Estimate=%lld\n", cpu_id, cinfo->next_estimate);
                        //scale down the read weights
                        initialize_weight_matrix(cinfo, false);
                        continue;
                    }

                    /* Update write bandwidth history and estimate (NEW - DUAL ESTIMATION) */
                    cinfo->write_event_hist[cinfo->ri] = cinfo->g_write_count_used;
                    cinfo->write_next_estimate = estimate(
                        cinfo->write_event_hist,
                        sizeof(cinfo->write_event_hist)/sizeof(cinfo->write_event_hist[0]),
                        cinfo->write_weight_matrix,
                        sizeof(cinfo->write_weight_matrix)/sizeof(cinfo->write_weight_matrix[0]),
                        cinfo->ri
                    ) + cinfo->initial_bw_mb;

                    /* Check for negative write estimate */
                    if(cinfo->write_next_estimate < 0){
                        AR_DEBUG("CPU(%u): Negative Write Estimate=%lld\n", cpu_id, cinfo->write_next_estimate);
                        //scale down the write weights
                        initialize_write_weight_matrix(cinfo, false);
                        continue;
                    }

                    /* Combined total bandwidth estimate (read + write) */
                    s64 total_next_estimate = cinfo->next_estimate + cinfo->write_next_estimate;

                    /* Separate allocation for read and write based on estimates */
                    u64 read_allocation, write_allocation;
                    
                    if (total_next_estimate <= cinfo->max_bw_limit_mb) {
                        /* Total estimate within limit - allocate as estimated */
                        read_allocation = cinfo->next_estimate;
                        write_allocation = cinfo->write_next_estimate;
                    } else {
                        /* Total estimate exceeds limit - allocate proportionally */
                        /* Ratio: read_allocation = max_bw * (read_estimate / total_estimate) */
                        /*        write_allocation = max_bw * (write_estimate / total_estimate) */
                        
                        read_allocation = (cinfo->max_bw_limit_mb * cinfo->next_estimate) / total_next_estimate;
                        write_allocation = (cinfo->max_bw_limit_mb * cinfo->write_next_estimate) / total_next_estimate;
                    }
                    
                    u64 total_allocation = read_allocation + write_allocation;
                    g_pool_bw_mb += max(0, cinfo->max_bw_limit_mb - total_allocation);

                    /* Allocate budget separately to read and write counters */
                    local64_set(&cinfo->read_event->hw.period_left, convert_mb_to_events(read_allocation));
                    local64_set(&cinfo->write_event->hw.period_left, convert_mb_to_events(write_allocation));

                    /* Unthrottle if the core is in throttle state */
                    atomic_set(&cinfo->throttler_task, false);

                    /* Re-enable the counters */
                    cinfo->read_event->pmu->start(cinfo->read_event, PERF_EF_RELOAD);
                    cinfo->write_event->pmu->start(cinfo->write_event, PERF_EF_RELOAD);
                    //cinfo->cycles_l3miss_event->pmu->start(cinfo->cycles_l3miss_event, PERF_EF_RELOAD);

                    /* Update weights separately for read and write */
                    s64 read_error = cinfo->g_read_count_used - cinfo->prev_estimate;
                    s64 write_error = cinfo->g_write_count_used - cinfo->write_prev_estimate;
                    
                    update_weight_matrix(read_error, cinfo);
                    update_write_weight_matrix(write_error, cinfo);

#if defined(AR_DEBUG)
                    char buf[HIST_SIZE][51]={0};
                    char wbuf[HIST_SIZE][51]={0};  // Write weights buffer
                    for (u8 i = 0; i < HIST_SIZE; i++){
                        kernel_fpu_begin();
                        print_double(buf[i],cinfo->weight_matrix[i]);          // Read weights
                        print_double(wbuf[i],cinfo->write_weight_matrix[i]);   // Write weights
                        kernel_fpu_end();
                    }
#endif
                    (cinfo->ri)++;
                    cinfo->ri = (cinfo->ri == HIST_SIZE)? 0:cinfo->ri;

                    AR_DEBUG("CPU(%u):r_bw_used=%llu r_est=%lld r_alloc=%llu w_bw_used=%llu w_est=%lld w_alloc=%llu \
                    total_est=%lld total_alloc=%llu r_err=%lld w_err=%lld Gpool=%lu net_bw_used=%llu \
                    rw0=%s rw1=%s rw2=%s rw3=%s rw4=%s ww0=%s ww1=%s ww2=%s ww3=%s ww4=%s\n",
                                 cpu_id,
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
                                 net_bandwidth_used,
                                 buf[0],buf[1],buf[2],buf[3],buf[4],      // Read weights (rw0-rw4)
                                 wbuf[0],wbuf[1],wbuf[2],wbuf[3],wbuf[4]  // Write weights (ww0-ww4)
								 );

                    /* Save current estimates for next iteration */
                    cinfo->prev_estimate = cinfo->next_estimate;
                    cinfo->write_prev_estimate = cinfo->write_next_estimate;
                    break;
                default:
                    continue;
            }
        }
       msleep(1);
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
