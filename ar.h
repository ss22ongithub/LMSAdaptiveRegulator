#if !defined AR_H
#define AR_H

#include "models/model-xgb.h"

#define HIST_SIZE 5
#define MAX_NO_CPUS 4

extern ulong g_total_available_bw_mb;

/* ML budget policy (module parameters, see ar.c). All in MB/s. */
extern ulong ml_initial_bw_mb;  /* warm-up budget and per-class floor        */
extern ulong ml_max_bw_mb;      /* per-core cap on read + write              */
extern ulong ml_scale_pct;      /* budget = prediction * scale% + headroom   */
extern ulong ml_headroom_mb;
extern bool ml_enable;          /* kill switch; 0 = static budgets, no FPU   */
extern int ml_selftest_state;   /* 0 not run, 1 passed, -1 failed (master.c) */
extern u8 g_num_regulated_cpus; /* CPU 0 is the master; 1..N are regulated   */

/**************************************************************************
 * COUNTERS Format (Umask_code - EventCode) tools/perf/pmu-events/arch/x86/)
 **************************************************************************/
#if defined(__aarch64__) || defined(__arm__)
#  define PMU_LLC_MISS_COUNTER_ID 0x17   // LINE_REFILL
#  define PMU_LLC_WB_COUNTER_ID   0x18   // LINE_WB
#elif defined(__x86_64__) || defined(__i386__)
#  define PMU_LLC_MISS_COUNTER_ID 0x08b0 // OFFCORE_REQUESTS.ALL_DATA_RD
#  define PMU_LLC_WB_COUNTER_ID   0x40b0 // OFFCORE_REQUESTS.WB

//CYCLE_ACTIVITY.STALLS_L3_MISS
#  define PMU_STALL_L3_MISS_CYCLES_COUNTER_ID   0x06A3
/*
* Event: CYCLE_ACTIVITY.STALLS_L3_MISS
* EventSel=A3H, UMask=06H, CMask=06H
*/
#define PMU_STALL_L3_MISS_CYCLES_EVENTSEL 0xA3
#define PMU_STALL_L3_MISS_CYCLES_UMASK    0x06


#endif


/* Each CPU core's info */
struct core_info {
    bool thr;
    u64 g_read_count_new;
    u64 g_read_count_old;
    u64 g_read_count_used;
    u64 read_limit;

    u64 g_write_count_new;
    u64 g_write_count_old;
    u64 g_write_count_used;

  // History of count of LLC read misses occurred between the regulation intervals.
  // (Event: Read misses)
  u64 read_event_hist[HIST_SIZE];
  
  // History of count of LLC write-backs occurred between the regulation intervals.
  // (Event: Write-backs)
  u64 write_event_hist[HIST_SIZE];
  
  u8 ri;  // Ring buffer index (shared for both read and write history)

  u8 cpu_id;
  wait_queue_head_t throttle_evt;
  /* UPDATE: Currently this variable unused and atomic throttle variable is used
   * instead. True = core in throttled state  False = core not throttled */
  atomic_t throttler_task;

  /*
   * Pointer to the throttler kthread context.
   */
  struct task_struct *throttler_thread;

  //  Bandwidth utilization parameters
  // PMC events. The two throttling events count what the ML model predicts
  // (per-core "calc" bandwidth, see ar.c):
  //   read_event  = cache-misses      (64 B lines read from memory)
  //   write_event = LLC-store-misses  (64 B store misses)
  struct perf_event *read_event;
  struct perf_event *write_event;

  // Timer related
  struct hrtimer reg_timer;
  struct irq_work read_irq_work;

  // Memory Bandwidth Budget estimate for the core.
  // Computed by master core
  atomic64_t budget_est;

  /* (The LMS weight matrices -- double arrays -- were removed: nothing uses
   * them in the ML copy, and no object other than models/model-xgb-fpu.o may hold
   * floating-point state.) */

  // Read bandwidth estimates (keep original names)
  s64 next_estimate;      // Next read bandwidth estimate
  s64 prev_estimate;      // Previous read bandwidth estimate
  
  // Write bandwidth estimates (NEW)
  s64 write_next_estimate;
  s64 write_prev_estimate;
  
  // Per-core bandwidth limits (for dynamic configuration)
  u64 initial_bw_mb;      // Initial/minimum bandwidth in MB/s
  u64 max_bw_limit_mb;    // Maximum bandwidth limit in MB/s

  /*
   * ---- ML predictor (replaces the LMS estimator in this copy) ----
   * The LMS history fields above are no longer used. next_estimate and
   * write_next_estimate (MiB/s) keep their meaning: they are the budgets the
   * 1 ms loop re-arms the counters with, now refreshed from the ML
   * prediction every 10 ms instead of by LMS every loop.
   */
  // Plain counting events for the other model inputs ([ML_RD] / [ML_WR]
  // stay NULL: those are read_event / write_event).
  struct perf_event *cnt_event[ML_N_CNT];
  bool ml_counters_ok;               // every model counter could be created

  // Written ONLY by ar_sample_and_rearm() on the core itself (IPI), read by
  // the master after the IPI returned:
  u64 snap[ML_N_CNT];                // cumulative counts at the snapshot
  u64 snap_ns;                       // ktime of the snapshot
  bool snap_all_active;              // every counter was running

  // Written by the master, applied on the core by the next IPI:
  u64 arm_rd_events, arm_wr_events;  // budget for the next 1 ms period

  struct ml_state ml;                // history ring + online accuracy
  bool ml_primed;                    // ml_prev / ml_prev_ns are valid
  u64 ml_prev[ML_N_CNT];             // snapshot at the start of the sample
  u64 ml_prev_ns;
  u32 ml_infer_ns;                   // duration of the last ml_predict()
  u32 ml_infer_max_ns;               // worst case seen
  u32 ipi_failures;                  // smp_call_function_single() errors

};

struct core_info *get_core_info(u8 cpu_id);
bool ar_ml_counters_available(void);  /* all model counters on all regulated cores */
void start_regulation(u8 cpu_id);
void stop_regulation(u8 cpu_id);

/* New wrapper functions for coordinated regulation start/stop */
void start_all_regulation(void);
void stop_all_regulation(void);


struct bw_distribution {
  u32 time;
  u32 rd_avg_bw;
};

void __throttle( void* cpu );
void __unthrottle( void* cpu );
#endif
