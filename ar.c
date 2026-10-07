/**
 * Dynamic adaptive memory bandwidth controller for multi-core systems
 *
 *
 * This file is distributed under GPL v2 License. 
 * See LICENSE.TXT for details.
 *
 */


/**************************************************************************
 * Included Files
 **************************************************************************/
#include "kernel_headers.h"
#include "ar.h"
#include "master.h"
#include "ar_debugfs.h"
#include "ar_perfs.h"
#include "utils.h"
#include "model.h"

/**************************************************************************
 * Public Definitions
 **************************************************************************/







/**************************************************************************
 * Local Function Declarations
 **************************************************************************/

static void deinitialize_cpu_info(const u8 cpu_id);
static int  setup_cpu_info(const u8 cpu_id);
static void ar_handle_read_overflow(struct irq_work *entry);


/**************************************************************************
 * Module parameters
**************************************************************************/

static int g_read_counter_id = PMU_LLC_MISS_COUNTER_ID;
module_param(g_read_counter_id, hexint,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

static int g_write_counter_id = PMU_LLC_WB_COUNTER_ID;
module_param(g_write_counter_id, hexint,  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);


//Default global pool is zero .It will be filled with unused bandwidths.
//Allow prefilling while loading module
ulong g_pool_bw_mb = 0;
module_param(g_pool_bw_mb, ulong, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

/**************************************************************************
 * Global Variables
 **************************************************************************/

u64 g_initial_bw_mb[MAX_NO_CPUS+1] = {0,500,500,500,500}; /*Pre-defined initial / min Bandwidth in MB/s */
const u64 g_percore_bw_limit_mb[MAX_NO_CPUS+1] = {0,1000,1000,1000,1000}; /*Pre-defined max Bandwidth per core in MB/s */

/* CPU 0 is always the master core (kernel ties boot/IRQ-affine duties to it,
 * so it's never a good regulation target); CPUs 1..g_num_regulated_cpus are
 * regulated. Computed once in ar_init() from num_online_cpus()-1, capped at
 * MAX_NO_CPUS since all_cinfo and the bandwidth tables are sized for that. */
u8 g_num_regulated_cpus;

/**************************************************************************
 * Static Types
 ***************************************************************************/

static struct core_info all_cinfo[MAX_NO_CPUS + 1];

/**************************************************************************
 * Utils
 **************************************************************************/

struct core_info* get_core_info(u8 cpu_id){
    /* CPU 0 is reserved for master thread, CPUs beyond MAX_NO_CPUS not supported */
    if (cpu_id == 0 || cpu_id > MAX_NO_CPUS) {
        pr_err("Invalid CPU ID %u (CPU 0 is master, max supported: %d)!!!", cpu_id, MAX_NO_CPUS);
        return NULL;
    }
    
    return &all_cinfo[cpu_id];
}




/* WARNING: This function should be kept strictly re-entrant */
void __throttle( void* cpu )
{
    /*Note : Ensure cinfo should always refer to the regulated cores */
    struct core_info* cinfo = (struct core_info*)cpu;
    bool t = atomic_read(&cinfo->throttler_task);

    AR_DEBUG("CPU(%d): throtle state = %d\n", cinfo->cpu_id, t);
    if ( t ) {
        AR_DEBUG("Already in throttled state");
        return;
    }
    atomic_set(&cinfo->throttler_task,true);
    wake_up_interruptible(&cinfo->throttle_evt);
}

/* WARNING: This function should be kept strictly re-entrant */
void __unthrottle( void* cpu) {
    /* Note : Ensure cinfo should always refer to the regulated cores */
    struct core_info* cinfo = (struct core_info*)cpu;
    atomic_set(&cinfo->throttler_task,false);
}

/* Throttles the CPU using architecture specific primitives. Otherwise
 * falls back to spinning on cpu_relax(). */
static inline void ar_throttle_cpu_wait(struct core_info *cinfo)
{
#if defined(__x86_64__) || defined(__i386__)
    if (!boot_cpu_has(X86_FEATURE_MWAIT)) {
        smp_mb();
        cpu_relax();
        return;
    }

    __monitor(&cinfo->throttler_task, 0, 0);
    smp_mb();
    if (atomic_read(&cinfo->throttler_task) && !kthread_should_stop())
        __mwait(0, 0);
#elif defined(__aarch64__) || defined(__arm__)
    wfe();
    smp_mb();
#else
    smp_mb();
    cpu_relax();
#endif
}

/**************************************************************************
 * Callbacks and Handlers
 **************************************************************************/


/**************************************************************************
 * Throttle Thread
 **************************************************************************/

static int throttler_task_func1(void * data){
    u8 cpu_id = (unsigned long)data;
    pr_info("%s: Enter CPU(%d)",__func__,cpu_id);

    struct core_info *cinfo = get_core_info(cpu_id);//per_cpu_ptr(core_info, cpunr);
    BUG_ON(cinfo == NULL);
    sched_set_fifo(current);

    while (!kthread_should_stop() && cpu_online(cpu_id)) {

        AR_DEBUG("CPU(%d):Waiting for Event\n", cpu_id);
        wait_event_interruptible(cinfo->throttle_evt,
                                 atomic_read(&cinfo->throttler_task)
                                 || kthread_should_stop() );
        AR_DEBUG("CPU(%d):Got Event\n", cpu_id);

        if (kthread_should_stop())
            break;

        AR_DEBUG("CPU(%d):Throttling with MWAIT...\n",cpu_id);

        while (atomic_read(&cinfo->throttler_task) && !kthread_should_stop())
            ar_throttle_cpu_wait(cinfo);

        AR_DEBUG("CPU(%d):Unthrottled\n",cpu_id);
    }

    pr_info("%s: Exit",__func__);
    return 0;
}

/* Callback when read counter exhuasts its budget*/
static void read_event_overflow_callback(struct perf_event *event,
                    struct perf_sample_data *data,
                    struct pt_regs *regs)
{
    u8 cpu_id = smp_processor_id();
    if (cpu_id == 0){
        AR_DEBUG("%s: CPU(%d) not expected here \n",__func__,cpu_id);
        return;
    }

    struct core_info *cinfo = get_core_info(cpu_id);
    BUG_ON(!cinfo);
    irq_work_queue(&cinfo->read_irq_work);
}

static void ar_handle_read_overflow(struct irq_work *entry)
{
    u8 cpu_id = smp_processor_id();
    if (cpu_id == 0){
        AR_DEBUG("%s: CPU(%d) not expected here\n",__func__,cpu_id);
        return;
    }
    BUG_ON(in_nmi() || !in_irq());

    struct core_info *cinfo = get_core_info(cpu_id);
    BUG_ON(!cinfo);
    u64 read_bw_used = cinfo->g_read_count_used;
    u64 reclaim_budget = 0;
    if (read_bw_used < g_pool_bw_mb )
    {
        reclaim_budget = read_bw_used/2;
        g_pool_bw_mb -=  reclaim_budget;
        local64_set(&cinfo->read_event->hw.period_left, reclaim_budget);
        AR_DEBUG("read_bw_used(%llu) < g_pool_bw_mb(%lu): reclaim_budget(%llu)", read_bw_used, g_pool_bw_mb, reclaim_budget);
        return;
    }

    //Activate Throttling
    atomic_set(&cinfo->throttler_task,true);
    wake_up_interruptible(&cinfo->throttle_evt);

}


/**************************************************************************
 * Other Utils
 **************************************************************************/

static int  setup_cpu_info(const u8 cpu_id){
    pr_info("%s: Enter CPU(%d)", __func__,cpu_id );
    struct core_info* cinfo = get_core_info(cpu_id);
    BUG_ON(cinfo==NULL);
    memset(cinfo, 0, sizeof(struct core_info));
    cinfo->cpu_id = cpu_id;

    /* Initialize per-core bandwidth limits from global arrays */
    cinfo->initial_bw_mb = g_initial_bw_mb[cpu_id];
    cinfo->max_bw_limit_mb = g_percore_bw_limit_mb[cpu_id];
    pr_info("CPU %d: initial_bw=%llu MB/s, max_bw=%llu MB/s\n", 
            cpu_id, cinfo->initial_bw_mb, cinfo->max_bw_limit_mb);

    cinfo->read_event =  init_counter(cinfo->cpu_id,
                                     convert_mb_to_events(g_initial_bw_mb[cpu_id]),
                                     g_read_counter_id,
                                     read_event_overflow_callback);
    if (cinfo->read_event == NULL){
        pr_err("Read_event %p did not allocate ", cinfo->read_event);
        return -1;
    }
    
    cinfo->write_event = init_counter(cinfo->cpu_id,
                                      convert_mb_to_events(g_initial_bw_mb[cpu_id]),
                                      PMU_LLC_WB_COUNTER_ID,
                                      NULL);  /* No callback for write - we don't throttle on write alone */
    if (cinfo->write_event == NULL){
        pr_err("Write_event %p did not allocate ", cinfo->write_event);
        return -1;
    }
    
    /* cycles_l3miss_event disabled
    cinfo->cycles_l3miss_event = init_counter(cinfo->cpu_id,6,
                                       PMU_STALL_L3_MISS_CYCLES_COUNTER_ID,
                                       NULL);

    if (cinfo->cycles_l3miss_event == NULL){
        pr_err("Cycles_l3miss_event %p did not allocate ", cinfo->cycles_l3miss_event);
        return -1;
    }
    */

    /* Initialize NMI irq_work_queue */
    init_irq_work(&cinfo->read_irq_work, ar_handle_read_overflow);

    /* Disable the throttle flag */
    atomic_set(&cinfo->throttler_task,false);   
    

    /* Initialize Wait queue for throttler */
    init_waitqueue_head(&cinfo->throttle_evt);

    /* TODO: Investigate kthread_run_on_cpu API which is  a convenience wrapper
     * for kthread_creat_on_node + kthread_bind + wake_up_process.
     * This has an issue the incorrect cpuid is displayed in the ps command.
     * This needs to be reconfirmed.
     * https://docs.kernel.org/next/driver-api/basics.html
     * */
#if defined(TODO)
    cinfo->throttler_thread = kthread_run_on_cpu(throttler_task_func1,
                                    (void *)((unsigned long)cpu_id),
                                    cpu_to_node(cpu_id),
                                    "areg_kthrottler/%u",cpu_id);

    BUG_ON(IS_ERR(cinfo->throttler_task));
#else
    cinfo->throttler_thread = kthread_create_on_node(throttler_task_func1,
                                    (void *)((unsigned long)cpu_id),
                                    cpu_to_node(cpu_id),
                                    "areg_kthrottler/%u",cpu_id);

    BUG_ON(IS_ERR(cinfo->throttler_thread));
    kthread_bind(cinfo->throttler_thread, cpu_id);
    wake_up_process(cinfo->throttler_thread);
#endif

    /***** Regulation to be started by setting
    /sys/kernel/debug/ar/enable_regulation to 1 ****/

    /* Initialize weight matrices for both read and write bandwidth estimation */
    initialize_weight_matrix(cinfo, true);        // Read BW weights
    initialize_write_weight_matrix(cinfo, true);  // Write BW weights

    pr_info("%s: Exit", __func__ );
    return 0;
}

static void deinitialize_cpu_info( const u8 cpu_id){

    pr_info("%s:Enter CPU (%d)",__func__, cpu_id );
    
    struct core_info *cinfo = get_core_info(cpu_id);
    
    BUG_ON(cinfo == NULL);
        
    /* As much as possible keep the de-initialization in the reverse sequence of initialization
     * Refer: setup_cpu_info()
     * */
    
    //End the throttle thread
    if(cinfo->throttler_thread){
        atomic_set(&cinfo->throttler_task, false);  // Unthrottle first
        wake_up_interruptible(&cinfo->throttle_evt); // Wake up
        msleep(10);  // Give time to wake

        kthread_stop(cinfo->throttler_thread);
        atomic_set(&cinfo->throttler_task,false);
        cinfo->throttler_thread = NULL;
    }
    
    //Free the perf event counters
    if (cinfo->read_event) {
        disable_event(cinfo->read_event);
        cinfo->read_event = NULL;
    }
    
    if (cinfo->write_event) {
        disable_event(cinfo->write_event);
        cinfo->write_event = NULL;
    }
    
    /* cycles_l3miss_event disabled
    if (cinfo->cycles_l3miss_event) {
        disable_event(cinfo->cycles_l3miss_event);
        cinfo->cycles_l3miss_event = NULL;
    }
    */
    
    pr_info("%s:Exit",__func__ );
}

void start_regulation(u8 cpu_id){
    struct core_info* cinfo = get_core_info(cpu_id);
    BUG_ON(cinfo==NULL);
    
    /* Initialize read estimates */
    cinfo->next_estimate = 0;
    cinfo->prev_estimate = 0;
    
    /* Initialize write estimates */
    cinfo->write_next_estimate = 0;
    cinfo->write_prev_estimate = 0;

    /* Enable perf events */
    enable_event(cinfo->read_event);
    enable_event(cinfo->write_event);
    //enable_event(cinfo->cycles_l3miss_event);

    /* Note: Timers are not used in this design.
     * Master thread controls regulation timer */

    pr_info("%s: Exit: (CPU %u)",__func__,cpu_id );
}

void stop_regulation(u8 cpu_id){
    struct core_info* cinfo = get_core_info(cpu_id);
    BUG_ON(cinfo==NULL);

    /* Disable perf events */
    perf_event_disable(cinfo->read_event);
    perf_event_disable(cinfo->write_event);
    //perf_event_disable(cinfo->cycles_l3miss_event);

    /* Note: Timers are not used in this design */
    pr_info("%s: Exit: (CPU %u)",__func__,cpu_id );
}

/* New wrapper function that coordinates regulation start */
void start_all_regulation(void){
    pr_info("%s: Starting regulation for all cores", __func__);
    
    /* Signal master thread to unthrottle cores and begin regulation */
    master_start_regulation();
    
    /* Enable perf counters for all cores */
    for (u8 cpu_id = 1; cpu_id <= g_num_regulated_cpus; cpu_id++) {
        start_regulation(cpu_id);
    }
    
    pr_info("%s: All cores regulation started", __func__);
}

void stop_all_regulation(void){
    pr_info("%s: Stopping regulation for all cores", __func__);
    
    /* Stop master thread regulation */
    master_stop_regulation();
    
    /* Disable perf counters for all cores */
    for (u8 cpu_id = 1; cpu_id <= g_num_regulated_cpus; cpu_id++) {
        stop_regulation(cpu_id);
    }
    
    pr_info("%s: All cores regulation stopped", __func__);
}

/**************************************************************************************************************************
 * Module main
 **************************************************************************************************************************/

static int __init ar_init (void ){

    pr_info("Supported CPUs: %d, online_cpus: %d\n", NR_CPUS, num_online_cpus());
//    pr_info("FPU supported : %d",kernel_fpu_available());
#if defined(__x86_64__) || defined(__i386__)
    if (boot_cpu_has(X86_FEATURE_MWAIT)) {
        pr_info("MWAIT instruction supported - using hardware-efficient throttling\n");
    } else {
        pr_warn("MWAIT instruction NOT supported - using fallback throttling method\n");
    }
#else
    pr_info("Non-x86 architecture detected - MWAIT not available\n");
#endif

    //Initialise core infos
    memset(all_cinfo, 0, sizeof(all_cinfo));

    /* CPU 0 is always reserved for the master thread; regulate the rest,
     * up to however many this board actually has (clamped to MAX_NO_CPUS,
     * which is what all_cinfo and the bandwidth tables are sized for). */
    g_num_regulated_cpus = num_online_cpus() - 1;
    if (g_num_regulated_cpus > MAX_NO_CPUS)
        g_num_regulated_cpus = MAX_NO_CPUS;
    if (g_num_regulated_cpus < 1) {
        pr_err("Need at least 2 online CPUs (1 master + >=1 regulated), found %d\n",
               num_online_cpus());
        return -ENODEV;
    }
    pr_info("Regulating %d of %d online CPUs (CPU 0 reserved for master)\n",
            g_num_regulated_cpus, num_online_cpus());

    for (u8 cpu_id = 1; cpu_id <= g_num_regulated_cpus; cpu_id++) {
        int ret = setup_cpu_info(cpu_id);
        if (ret != 0) {
            pr_err("setup_cpu() Failed for CPU %d", cpu_id);
            for (int rollback_id = cpu_id - 1; rollback_id >= 1; rollback_id--)
                deinitialize_cpu_info((u8)rollback_id);
            return -ENOMEM;
        }
    }

    /* Initialize the master thread - this will throttle all cores */
    initialize_master();

    /* Create entries in debugfs */
    ar_init_debugfs();

    pr_info("Module Initialized - All cores throttled, waiting for regulation start\n");
    return 0;

}


static void __exit ar_exit( void )
{
    /* Keep the deinitializing sequence reverse of the allocation sequence seen in  __init function */
    ar_remove_debugfs();
    deinitialize_master();

    for (int cpu_id = g_num_regulated_cpus; cpu_id >= 1; cpu_id--)
        deinitialize_cpu_info((u8)cpu_id);

    pr_info("Module removed\n");
	return;
}

module_init(ar_init);
module_exit(ar_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sudarshan S <sudarshan.srinivasan@research.iiit.ac.in>");
