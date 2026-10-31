#if !defined AR_H
#define AR_H

#include "model.h"

#define MAX_NO_CPUS 4

extern ulong g_total_available_bw_mb;

/**************************************************************************
 * COUNTERS Format (Umask_code - EventCode) tools/perf/pmu-events/arch/x86/)
 **************************************************************************/
#if defined(__aarch64__) || defined(__arm__)
#  define PMU_LLC_MISS_COUNTER_ID 0x17   // LINE_REFILL
#  define PMU_LLC_WB_COUNTER_ID   0x18   // LINE_WB

/* Remaining ARMv8 PMUv3 events used as MLP features (EVENTS in
 * mlp_bw_predictor.py). CPU_CYCLES goes on the dedicated cycle counter, so
 * all 7 features fit on a Cortex-A72 (6 programmable + cycle counter). */
#  define PMU_CPU_CYCLES_ID       0x11
#  define PMU_INST_RETIRED_ID     0x08
#  define PMU_L1I_CACHE_ID        0x14
#  define PMU_BUS_ACCESS_ID       0x19  
#  define PMU_BUS_CYCLES_ID       0x1D
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

#if !defined(__aarch64__)
#  error "The MLP model is trained on ARMv8 PMU events; build for arm64"
#endif


/* Running totals since module load, exported via /sys/kernel/debug/ar/stats.
 * Event counts are user-mode only, like the MLP features. Written by the
 * master thread (counts) and the core's overflow handler (reclaims/throttles). */
struct ar_stats {
    u64 ticks;          // Regulation intervals measured
    u64 cycles;         // CPU_CYCLES
    u64 instructions;   // INST_RETIRED
    u64 l2_refill;      // L2D_CACHE_REFILL (read lines)
    u64 l2_wb;          // L2D_CACHE_WB (write lines)
    u64 reclaims;       // Read overflows served from the pool
    u64 throttles;      // Read overflows that throttled the core
};

/* Single-writer update that concurrent debugfs readers never see torn */
static inline void stat_add(u64 *stat, u64 v){
    WRITE_ONCE(*stat, *stat + v);
}

/* Each CPU core's info */
struct core_info {
    bool thr;
    u64 g_read_count_used;  // Read bandwidth used in the last interval (MB/s)
    u64 read_limit;

    u64 g_write_count_used; // Write bandwidth used in the last interval (MB/s)

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
  // PMC events
  struct perf_event *read_event;
  struct perf_event *write_event;
  struct perf_event *cycles_l3miss_event;

  /* All MLP feature counters, in enum mlp_feature order.
   * feat_event[MLP_EV_L2D_REFILL] and [MLP_EV_L2D_WB] alias read_event and
   * write_event; the other entries are counting-only events owned here. */
  struct perf_event *feat_event[MLP_N_EV];
  u64 feat_count_prev[MLP_N_EV];    // Counter values at the previous tick
  u64 feat_running_prev[MLP_N_EV];  // Counter running times (ns) at the previous tick
  bool primed;                      // Baselines above are valid
  struct ar_stats stats;

  // Timer related
  struct hrtimer reg_timer;
  struct irq_work read_irq_work;

  // Memory Bandwidth Budget estimate for the core.
  // Computed by master core
  atomic64_t budget_est;
  // Total (read + write) bandwidth estimates in MB/s
  u64 next_estimate;      // Budget for the next interval
  u64 prev_estimate;      // Budget given for the interval just measured
  
  // Per-core bandwidth limits (for dynamic configuration)
  u64 initial_bw_mb;      // Initial/minimum bandwidth in MB/s
  u64 max_bw_limit_mb;    // Maximum bandwidth limit in MB/s
  
};

struct core_info *get_core_info(u8 cpu_id);
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
