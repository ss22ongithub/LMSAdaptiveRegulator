//
// Created by ss22 on 25/9/25.
//
#include "kernel_headers.h"
#include "utils.h"

void print_double(char* buf, double value)
{
    int digits;
    int i;
    int k;

    /*Extract the sign*/
    i=0;
    if (value < 0 ) {
        buf[i++] = '-';
        value = value *(-1);
    }

    /*Count the number of digits before the decimal point*/
    digits = 1;
    while (value >= 10){
        value /= 10; digits++;
    }
//  WARN_ON(digits <= DOUBLE_LEN - 2);

    for (k = 0; k < DOUBLE_LEN - 2; k++)
    {
        buf[i] = '0';
        while (value >= 1 && buf[i] < '9')
        {
            buf[i]++;
            value -= 1;
        }
        i++;
        digits--;
        if (digits == 0)
        {
            buf[i] = '.';
            i++;
        }
        value *= 10;
    }
//  WARN_ON(i == DOUBLE_LEN - 1);
    buf[i] = 0;
}

u64 convert_events_to_mb(u64 events)
{
/*
 * BW  = (events * CACHE_LINE_SIZE) / time_in_us                        - bytes/us
 *     = (events * CACHE_LINE_SIZE * 1000000) / (time_in_us * 1024 * 1024)  - MiB/s
 * Rounded up. events is a per-interval delta, so the product cannot overflow.
 */
    u64 divisor = (u64)get_regulation_time() * 1024 * 1024;
    return div64_u64(events * CACHE_LINE_SIZE * USEC_PER_SEC + (divisor - 1), divisor);
}

u64 convert_mb_to_events(u64 mb)
{
    return div64_u64(mb * 1024 * 1024 * get_regulation_time(),
                     CACHE_LINE_SIZE * USEC_PER_SEC);
}
/* Placeholder for any utility function used adaptive regulation */
