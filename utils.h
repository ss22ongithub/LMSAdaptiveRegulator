//
// Created by ss22 on 25/9/25.
//

/* Placeholder for any utility function used adaptive regulation */

#ifndef ADAPTIVEREGULATOR_UTILS_H
#define ADAPTIVEREGULATOR_UTILS_H


#define CACHE_LINE_SIZE 64
extern u32  get_regulation_time(void);
/** convert MB/s to #of events (i.e., LLC miss counts) per 1ms */
u64 convert_mb_to_events(u64 mb);

/* Convert # of events to MB/s */
u64 convert_events_to_mb(u64 events);

//static inline void print_current_context(void)
//{
//    AR_DEBUG("in_interrupt(%ld)(hard(%ld),softirq(%d)"
//                 ",in_nmi(%d)),irqs_disabled(%d)\n",
//                 in_interrupt(), in_irq(), (int)in_softirq(),
//                 (int)in_nmi(), (int)irqs_disabled());
//}

#define PRECISION 8
#define DOUBLE_LEN (PRECISION + 3)
void print_double(char* buf, double value);


#endif //ADAPTIVEREGULATOR_UTILS_H
