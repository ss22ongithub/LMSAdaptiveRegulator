#ifndef AR_PERFS_H
#define AR_PERFS_H


struct perf_event *init_counter(int cpu, int sample_period, int counter_id, void *callback);

/* Same, for any perf type (PERF_TYPE_HARDWARE / HW_CACHE / RAW). A
 * sample_period of 0 makes a plain counting event (no overflow interrupt). */
struct perf_event *init_counter_typed(int cpu, u32 type, u64 config,
                                      u64 sample_period, void *callback);

/* Cumulative count of an event that is not stopped between reads. Reads the
 * counter on the event's own CPU, so it MAY SLEEP: never call it with
 * preemption disabled or inside a kernel_fpu_begin()/end() section. */
u64 perf_event_read_total(struct perf_event *event);

void event_read_overflow_callback(struct perf_event *event,
                    struct perf_sample_data *data,
                    struct pt_regs *regs);

inline u64 perf_event_count(struct perf_event *event);

inline void enable_event(struct perf_event *event);

inline void disable_event(struct perf_event *event);

void init_perf_workq(u8 cpuid);

/* Getter setters for read event */

struct perf_event* get_read_event(void);

void set_read_event(struct perf_event* event);


//Debug functions 
u64 get_llc_ofc(void);


#endif /*AR_PERFS_H*/
