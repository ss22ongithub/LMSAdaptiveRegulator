//
// Created by ss22 on 3/7/25.
//

#ifndef ADAPTIVEREGULATOR_KERNEL_HEADERS_H
#define ADAPTIVEREGULATOR_KERNEL_HEADERS_H

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/smp.h> /* IPI calls */
#include <linux/irq_work.h>
#include <linux/hardirq.h>
#include <linux/perf_event.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <asm/atomic.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/interrupt.h>
#include <linux/trace_events.h>
#include <linux/cpumask.h>
#include <linux/topology.h>
#include <linux/kfifo.h>
#include <linux/init.h>
#include <linux/hw_breakpoint.h>
#include <linux/kstrtox.h>
#include <linux/math64.h>

#if defined(__aarch64__) || defined(__arm__)
#include <asm/fpu.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <asm/fpu/api.h>
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 0, 0)
#  include <uapi/linux/sched/types.h>
#elif LINUX_VERSION_CODE > KERNEL_VERSION(4, 13, 0)
#  include <linux/sched/types.h>
#elif LINUX_VERSION_CODE > KERNEL_VERSION(3, 8, 0)
#  include <linux/sched/rt.h>
#endif
#include <linux/sched.h>

#if defined CONFIG_DEBUG_AR
#define AR_DEBUG(fmt, ...) trace_printk(pr_fmt(fmt), ##__VA_ARGS__)
#else
#define AR_DEBUG(fmt, ...) do { } while (0)
#endif

/* x86 MWAIT/MONITOR support */
#if defined(__x86_64__) || defined(__i386__)
#include <asm/mwait.h>
#include <asm/cpufeature.h>
#endif


#endif //ADAPTIVEREGULATOR_KERNEL_HEADERS_H
