/**************************************************************************
 * Included Files
 **************************************************************************/

#include "kernel_headers.h"
#include "ar.h"
#include "ar_perfs.h"

/**************************************************************************
 * Perf Structure definitions 
 **************************************************************************/


static struct perf_event *llc_miss_read_event=NULL;
static struct irq_work llc_miss_event_irq_work;



/**************************************************************************
 * Callbacks and Handlers
 **************************************************************************/
static u64 llc_overflow_count =0 ;
static void llc_miss_event_irq_work_handler(struct irq_work *entry){
    
    BUG_ON(in_nmi() || !in_hardirq());
//    struct core_info * cinfo = get_core_info(cpu_id);
    // s64 read_budget_used = llc_overflow_count= perf_event_count(llc_miss_read_event) - (cinfo->g_read_count_old);

    // AR_DEBUG("read_budget_used %lld\n",read_budget_used);

    /* Send notfication to throttle_thread  to start the throttling*/
//    cinfo->throttler_task = current;
//    wake_up_interruptible(&cinfo->throttle_evt);

}
u64 get_llc_ofc(void){
    return llc_overflow_count;
}

/**************************************************************************
 * Perf Utils
 **************************************************************************/

/** read current counter value. */
inline u64 perf_event_count(struct perf_event *event)
{
    return local64_read(&event->count) + 
        atomic64_read(&event->child_count);
}

/** Cumulative count of an event that keeps running (see ar_perfs.h). */
u64 perf_event_read_total(struct perf_event *event)
{
    u64 enabled, running;

    if (!event)
        return 0;
    return perf_event_read_value(event, &enabled, &running);
}

struct perf_event *init_counter(int cpu, int sample_period, int counter_id, void *callback)
{
    return init_counter_typed(cpu, PERF_TYPE_RAW, (u64)counter_id,
                              (u64)sample_period, callback);
}

struct perf_event *init_counter_typed(int cpu, u32 type, u64 config,
                                      u64 sample_period, void *callback)
{
    struct perf_event *event = NULL;
    struct perf_event_attr sched_perf_hw_attr = {
        .type       = type,
        .size       = sizeof(struct perf_event_attr),
        .pinned     = 1,
        .disabled   = 1,
        .sample_period = sample_period,
        .config         = config,
        .exclude_kernel = 1   /* TODO: 1 mean, no kernel mode counting */
    };

    /*
     * If we are using the STALLS_L3_MISS event, we MUST set the CMask
     * to get the correct behavior (stalls > 6 cycles).
     */

//    if (counter_id == PMU_STALL_L3_MISS_CYCLES_COUNTER_ID) {
//    /*
//     * This is the critical part from the Intel documentation.
//     * We must pack all values into the 64-bit config field.
//     *
//     * config[ 7: 0] = EventSel (0xA3)
//     * config[15: 8] = UMask (0x06)
//     * config[31:24] = CMask (0x06)
//     */
//        sched_perf_hw_attr.config = PMU_STALL_L3_MISS_CYCLES_EVENTSEL;
//        sched_perf_hw_attr.config |= (__u64)PMU_STALL_L3_MISS_CYCLES_UMASK << 8;
//        sched_perf_hw_attr.config |= (__u64)PMU_STALL_L3_MISS_CYCLES_CMASK << 24;
//    }

    /* Try to register using hardware perf events */
    event = perf_event_create_kernel_counter(
        &sched_perf_hw_attr,
        cpu, /* CPU */
        NULL,   /* struct task_struct *task */
        callback,
        NULL);  /* void *context */

    if (!event)
        return NULL;

    if (IS_ERR(event)) {
        /* vary the KERN level based on the returned errno */
        if (PTR_ERR(event) == -EOPNOTSUPP)
            pr_info("cpu%d. not supported\n", cpu);
        else if (PTR_ERR(event) == -ENOENT)
            pr_info("cpu%d. not h/w event\n", cpu);
        else
            pr_err("cpu%d. unable to create perf event: %ld\n",
                   cpu, PTR_ERR(event));
        return NULL;
    }

    pr_info("CPU%d configured counter type=%u config=0x%llx\n", cpu, type, config);
    return event;
}


void event_read_overflow_callback(struct perf_event *event,
                    struct perf_sample_data *data,
                    struct pt_regs *regs)
{
    irq_work_queue(&llc_miss_event_irq_work);
}


void enable_event(struct perf_event *event){

	perf_event_enable(event);
    pr_info("Perf event enabled (%llx)\n", event->attr.config);
}

void disable_event(struct perf_event *event){
    /* Read the config before releasing: the original printed
     * event->attr.config after perf_event_release_kernel() (use after free). */
    const u64 config = event->attr.config;

    perf_event_disable(event);
    perf_event_release_kernel(event);
    pr_info("Perf event disabled (%llx)\n", config);

}

void init_perf_workq(u8 cpu_id){
	/* initialize irq_work_queue */
	pr_info("%s: Enter ",__func__);
    init_irq_work(&llc_miss_event_irq_work, llc_miss_event_irq_work_handler);
    pr_info("%s: Exit ",__func__);

}

struct perf_event* get_read_event(){
    return llc_miss_read_event;
}

void set_read_event(struct perf_event* e){
    llc_miss_read_event  = e;
}