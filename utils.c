//
// Created by ss22 on 25/9/25.
//
#include "kernel_headers.h"
#include "utils.h"

/*
 * print_double() was removed in the ML copy: it did `double` arithmetic, and
 * this object is deliberately built without SSE (only models/model-xgb-fpu.o may contain
 * floating-point code). Nothing used it any more once the LMS weight dump in
 * master.c was dropped.
 */

u64 convert_events_to_mb(u64 events)
{
/*
 * BW  = (event * CACHE_LINE_SIZE)/ time_in_ms  - bytes/ms
 *     =  (event * CACHE )/ (time_in_ms * 1024 *1024)  = mb/ms
 *     =  (event * CACHE * 1000 )/ (time_in_sec * 1024 *1024)  = mb/s
 */
    int divisor = get_regulation_time()*1024*1024;
    int mb = div64_u64(events*CACHE_LINE_SIZE*1000 + (divisor-1), divisor);
    return mb;
	/* The bandwidth computed here is has units Mib/msec but it is scaled by a factor of 10000.  */
}

u64 convert_mb_to_events(u64 mb)
{
    return div64_u64((u64)mb*1024*1024,
                     CACHE_LINE_SIZE * (1000/get_regulation_time()));
}
/* Placeholder for any utility function used adaptive regulation */
