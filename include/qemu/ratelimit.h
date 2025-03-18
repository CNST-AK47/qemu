/*
 * Ratelimiting calculations
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi   <stefanha@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#ifndef QEMU_RATELIMIT_H
#define QEMU_RATELIMIT_H

#include "qemu/lockable.h"
#include "qemu/timer.h"

typedef struct {
    QemuMutex lock;
    // 时间片开始时间
    int64_t slice_start_time;
    // 时间片结束时间
    int64_t slice_end_time;
    // 剩余时间片quota
    uint64_t slice_quota;
    // 时间片ns
    uint64_t slice_ns;
    // 对应派遣分数--用于统计当前已经执行数量
    uint64_t dispatched;
} RateLimit;

/** Calculate and return delay for next request in ns
 * 获取对应延迟时间
 * Record that we sent @n data units (where @n matches the scale chosen
 * during ratelimit_set_speed). If we may send more data units
 * in the current time slice, return 0 (i.e. no delay). Otherwise
 * return the amount of time (in ns) until the start of the next time
 * slice that will permit sending the next chunk of data.
 *
 * Recording sent data units even after exceeding the quota is
 * permitted; the time slice will be extended accordingly.
 */
static inline int64_t ratelimit_calculate_delay(RateLimit *limit, uint64_t n)
{
    // 获取当前时间
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    double delay_slices;
    // 进行加锁
    QEMU_LOCK_GUARD(&limit->lock);
    if (!limit->slice_quota) {
        /* Throttling disabled.  */
        return 0;
    }
    assert(limit->slice_ns);
    // 限流已经结束--进行重置
    // 一般为触发限流后第二次进入此逻辑
    if (limit->slice_end_time < now) {
        /* Previous, possibly extended, time slice finished; reset the
         * accounting. */
        limit->slice_start_time = now;
        limit->slice_end_time = now + limit->slice_ns;
        limit->dispatched = 0;
    }
    // 计算派遣时间
    limit->dispatched += n;
    // 未超过时间片限制，直接no delay
    if (limit->dispatched < limit->slice_quota) {
        /* We may send further data within the current time slice, no
         * need to delay the next request. */
        return 0;
    }

    /* Quota exceeded. Wait based on the excess amount and then start a new
     * slice. */
    // 获取需要延迟的时间片 当前读取量/限制量 --需要延迟的时间
    // 比如限制 10MB/s, 当前已经读取了50MB，则需要再等待5s
    delay_slices = (double)limit->dispatched / limit->slice_quota;
    limit->slice_end_time = limit->slice_start_time +
        (uint64_t)(delay_slices * limit->slice_ns);
    // 计算需要延迟的时间片
    return limit->slice_end_time - now;
}

static inline void ratelimit_init(RateLimit *limit)
{
    qemu_mutex_init(&limit->lock);
}

static inline void ratelimit_destroy(RateLimit *limit)
{
    qemu_mutex_destroy(&limit->lock);
}
/**
 * @brief  设置限制时间片速度
 * @param  limit            限制器
 * @param  speed            io 速度
 * @param  slice_ns         
 */
static inline void ratelimit_set_speed(RateLimit *limit, uint64_t speed,
                                       uint64_t slice_ns)
{
    QEMU_LOCK_GUARD(&limit->lock);
    limit->slice_ns = slice_ns;
    if (speed == 0) {
        limit->slice_quota = 0;
    } else {
        // 设置最大的时间片quota
        limit->slice_quota = MAX(((double)speed * slice_ns) / 1000000000ULL, 1);
    }
}

#endif
