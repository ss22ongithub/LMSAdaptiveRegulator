/*
 * kstub.h -- a small APPROXIMATION of the Linux kernel API, used only to
 * run `gcc -fsyntax-only` over the kernel-side sources on a machine without
 * kernel headers (see host/syntax_check.py). Passing proves the code has no
 * typos, wrong field names or type mismatches against this approximation.
 * It does NOT prove it builds against a real kernel: that is `make` on the
 * Linux box. Signatures follow Linux 6.8 as far as the module uses them.
 */
#ifndef KSTUB_H
#define KSTUB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>

/* The kernel redefines `inline` as gnu_inline (compiler_types.h): a plain
 * `inline` definition then also emits an external symbol. The module relies
 * on that for perf_event_count()/enable_event()/disable_event(). */
#define inline inline __attribute__((gnu_inline))

typedef uint8_t u8;   typedef uint16_t u16;  typedef uint32_t u32;  typedef uint64_t u64;
typedef int8_t s8;    typedef int16_t s16;   typedef int32_t s32;   typedef int64_t s64;
typedef unsigned long ulong;
typedef unsigned int uint;
typedef long long loff_t;

#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6, 8, 0)

#define __user
#define __init
#define __exit
#define __maybe_unused __attribute__((unused))
#define NSEC_PER_MSEC 1000000ULL

#ifndef S_IRUSR
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IROTH 0004
#endif

/* ---- printk & friends ---- */
int __kstub_printk(const char *fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
#define pr_info(fmt, ...) __kstub_printk(pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)  __kstub_printk(pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...) __kstub_printk(pr_fmt(fmt), ##__VA_ARGS__)
#define WARN_ON(x) ((void)(x))
#define BUG_ON(x)  ((void)(x))
#define MODULE_DESCRIPTION(s)
void cond_resched(void);
#define EIO 5
#define ENODEV 19
#define ENOMEM 12
#define EINVAL 22
#define EOPNOTSUPP 95
#define ENOENT 2

/* ---- math / helpers ---- */
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define clamp_t(t, v, lo, hi) ((t)(v) < (t)(lo) ? (t)(lo) : ((t)(v) > (t)(hi) ? (t)(hi) : (t)(v)))
#define max(a, b) ((a) > (b) ? (a) : (b))
u64 div64_u64(u64 a, u64 b);
s64 div_s64(s64 a, s32 b);
u64 mul_u64_u64_shr(u64 a, u64 b, unsigned int shift);
u64 mul_u64_u64_div_u64(u64 a, u64 b, u64 c);
#define IS_ERR(p) ((unsigned long)(p) >= (unsigned long)-4095)
#define PTR_ERR(p) ((long)(p))

/* ---- atomics ---- */
typedef struct { int counter; } atomic_t;
typedef struct { s64 counter; } atomic64_t;
typedef struct { s64 a; } local64_t;
#define ATOMIC_INIT(i) { (i) }
int atomic_read(const atomic_t *v);
void atomic_set(atomic_t *v, int i);
s64 atomic64_read(const atomic64_t *v);
s64 local64_read(const local64_t *l);
void local64_set(local64_t *l, s64 v);
void smp_mb(void);
void cpu_relax(void);

/* ---- CPUs, threads, waiting ---- */
#define NR_CPUS 8
#define for_each_online_cpu(cpu) for ((cpu) = 0; (cpu) < NR_CPUS; (cpu)++)
int num_online_cpus(void);
bool cpu_online(unsigned int cpu);
int cpu_to_node(int cpu);
unsigned int smp_processor_id(void);
bool in_nmi(void);
bool in_irq(void);
void msleep(unsigned int ms);
void usleep_range(unsigned long min, unsigned long max);
u64 ktime_get_ns(void);
typedef void (*smp_call_func_t)(void *info);
int smp_call_function_single(int cpu, smp_call_func_t func, void *info, int wait);

struct task_struct { int pid; };
extern struct task_struct *current;
struct task_struct *kthread_create_on_node(int (*fn)(void *), void *data, int node, const char *fmt, ...);
void kthread_bind(struct task_struct *t, unsigned int cpu);
int wake_up_process(struct task_struct *t);
int kthread_stop(struct task_struct *t);
bool kthread_should_stop(void);
void sched_set_fifo(struct task_struct *t);

typedef struct { int dummy; } wait_queue_head_t;
void init_waitqueue_head(wait_queue_head_t *q);
void wake_up_interruptible(wait_queue_head_t *q);
#define wait_event_interruptible(q, cond) ({ (void)&(q); (void)(cond); 0; })

struct irq_work { int dummy; };
void init_irq_work(struct irq_work *w, void (*fn)(struct irq_work *));
bool irq_work_queue(struct irq_work *w);
struct hrtimer { int dummy; };

/* ---- FPU ---- */
void kernel_fpu_begin(void);
void kernel_fpu_end(void);
bool irq_fpu_usable(void);
#define READ_ONCE(x)  (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))

/* ---- x86 MWAIT ---- */
#define X86_FEATURE_MWAIT 1
bool boot_cpu_has(int f);
void __monitor(const void *eax, unsigned long ecx, unsigned long edx);
void __mwait(unsigned long eax, unsigned long ecx);

/* ---- perf ---- */
#define PERF_TYPE_HARDWARE 0
#define PERF_TYPE_HW_CACHE 3
#define PERF_TYPE_RAW 4
#define PERF_COUNT_HW_CPU_CYCLES 0
#define PERF_COUNT_HW_INSTRUCTIONS 1
#define PERF_COUNT_HW_CACHE_REFERENCES 2
#define PERF_COUNT_HW_CACHE_MISSES 3
#define PERF_COUNT_HW_BRANCH_MISSES 5
#define PERF_COUNT_HW_CACHE_LL 2
#define PERF_COUNT_HW_CACHE_OP_READ 0
#define PERF_COUNT_HW_CACHE_OP_WRITE 1
#define PERF_COUNT_HW_CACHE_RESULT_MISS 1
#define PERF_EF_UPDATE 0x04
#define PERF_EF_RELOAD 0x01

struct perf_sample_data;
struct pt_regs;
struct perf_event;
struct perf_event_attr {
    u32 type; u32 size; u64 config; u64 sample_period;
    u64 pinned : 1, disabled : 1, exclude_kernel : 1;
};
struct hw_perf_event { local64_t period_left; };
struct pmu {
    void (*start)(struct perf_event *event, int flags);
    void (*stop)(struct perf_event *event, int flags);
    void (*read)(struct perf_event *event);
};
enum perf_event_state {
    PERF_EVENT_STATE_DEAD = -4, PERF_EVENT_STATE_EXIT = -3, PERF_EVENT_STATE_ERROR = -2,
    PERF_EVENT_STATE_OFF = -1, PERF_EVENT_STATE_INACTIVE = 0, PERF_EVENT_STATE_ACTIVE = 1,
};
struct perf_event {
    struct perf_event_attr attr;
    struct pmu *pmu;
    struct hw_perf_event hw;
    local64_t count;
    atomic64_t child_count;
    enum perf_event_state state;
    int oncpu;
};
typedef void (*perf_overflow_handler_t)(struct perf_event *, struct perf_sample_data *, struct pt_regs *);
struct perf_event *perf_event_create_kernel_counter(struct perf_event_attr *attr, int cpu,
        struct task_struct *task, perf_overflow_handler_t overflow_handler, void *context);
void perf_event_enable(struct perf_event *event);
int perf_event_disable(struct perf_event *event);
int perf_event_release_kernel(struct perf_event *event);
u64 perf_event_read_value(struct perf_event *event, u64 *enabled, u64 *running);

/* ---- module glue ---- */
#define module_param(name, type, perm) static const int __kstub_param_##name = (perm)
#define MODULE_PARM_DESC(n, d)
#define MODULE_LICENSE(s)
#define MODULE_AUTHOR(s)
#define module_init(f) static int (*__kstub_init_fn)(void) = f
#define module_exit(f) static void (*__kstub_exit_fn)(void) = f

/* ---- debugfs / seq_file ---- */
struct dentry;
struct inode;
struct file;
struct seq_file;
struct file_operations {
    int (*open)(struct inode *, struct file *);
    ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
    ssize_t (*read)(struct file *, char *, size_t, loff_t *);
    loff_t (*llseek)(struct file *, loff_t, int);
    int (*release)(struct inode *, struct file *);
};
struct dentry *debugfs_create_dir(const char *name, struct dentry *parent);
struct dentry *debugfs_create_file(const char *name, unsigned short mode, struct dentry *parent,
                                   void *data, const struct file_operations *fops);
void debugfs_remove_recursive(struct dentry *d);
int single_open(struct file *f, int (*show)(struct seq_file *, void *), void *data);
int single_release(struct inode *i, struct file *f);
ssize_t seq_read(struct file *, char *, size_t, loff_t *);
loff_t seq_lseek(struct file *, loff_t, int);
void seq_printf(struct seq_file *m, const char *fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
void seq_puts(struct seq_file *m, const char *s);
unsigned long copy_from_user(void *to, const void __user *from, unsigned long n);
int kstrtou32(const char *s, unsigned int base, u32 *res);
int kstrtou8(const char *s, unsigned int base, u8 *res);

#endif /* KSTUB_H */
