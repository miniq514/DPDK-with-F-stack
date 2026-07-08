/*
 * include/udp_rx_timing.h
 *
 * 收端突发窗口计时：首包 payload 成功 submit → 末包 submit。
 * DPDK / Linux 共用，用于丢包或未收满时仍能输出有效吞吐。
 */
#ifndef UDP_RX_TIMING_H
#define UDP_RX_TIMING_H

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <time.h>

struct udp_rx_burst_timing {
    uint64_t first_ns;      /* 首个成功 submit 的时刻 */
    uint64_t last_ns;       /* 最近成功 submit 的时刻 */
    uint32_t highest_seq;   /* 已成功 submit 的最大 seq */
    int      active;
};

static inline uint64_t udp_rx_monotonic_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void udp_rx_burst_on_submit(struct udp_rx_burst_timing *bt, uint32_t seq)
{
    uint64_t now = udp_rx_monotonic_ns();

    if (!bt->active) {
        bt->first_ns = now;
        bt->active = 1;
    }
    bt->last_ns = now;
    bt->highest_seq = seq;
}

static inline double udp_rx_burst_elapsed_sec(const struct udp_rx_burst_timing *bt)
{
    if (!bt->active || bt->last_ns <= bt->first_ns)
        return 0.0;
    return (double)(bt->last_ns - bt->first_ns) / 1e9;
}

/* 单包时避免除零；多包窗口仍用真实间隔 */
static inline double udp_rx_effective_burst_sec(const struct udp_rx_burst_timing *bt)
{
    double e = udp_rx_burst_elapsed_sec(bt);

    if (e >= 1e-9)
        return e;
    return bt->active ? 1e-9 : 0.0;
}

static inline void udp_rx_print_timing(uint32_t expected_pkts,
                                       const struct udp_rx_burst_timing *burst,
                                       uint64_t total_rx, uint64_t total_rx_bytes,
                                       uint64_t matched, uint64_t payload_bytes,
                                       double total_elapsed_sec)
{
    double burst_sec = burst->active ? udp_rx_effective_burst_sec(burst) : 0.0;

    if (expected_pkts > 0) {
        if (matched >= expected_pkts)
            printf("  Run status           : %12s\n", "COMPLETE");
        else
            printf("  Run status           : %12s  (%" PRIu64 "/%u pkts)\n",
                   "INCOMPLETE", matched, expected_pkts);
    }

    if (burst->active)
        printf("  Highest seq written  : %12u\n", burst->highest_seq);

    printf("  ------------------------------------------\n");

    if (burst->active && burst_sec > 0.0) {
        double wire_mbps = total_rx_bytes * 8.0 / burst_sec / 1e6;
        double wire_kpps = (double)total_rx / burst_sec / 1e3;
        double payload_mbps = payload_bytes * 8.0 / burst_sec / 1e6;
        double payload_kpps = (double)matched / burst_sec / 1e3;

        printf("  Elapsed time (burst) : %12.3f s  (first->last payload submit, seq 0->%u)\n",
               burst_sec, burst->highest_seq);
        if (total_elapsed_sec > burst_sec + 0.05)
            printf("  Elapsed time (total) : %12.2f s  (incl. startup/flush/wait)\n",
                   total_elapsed_sec);
        printf("  Throughput (burst, wire)    : %10.2f Mbps / %.2f Kpps\n",
               wire_mbps, wire_kpps);
        printf("  Throughput (burst, payload) : %10.2f Mbps / %.2f Kpps\n",
               payload_mbps, payload_kpps);
    } else {
        printf("  Elapsed time (burst) : %12s\n", "n/a");
        if (total_elapsed_sec > 0.0)
            printf("  Elapsed time (total) : %12.2f s\n", total_elapsed_sec);
        printf("  Throughput (burst, wire)    : %10s\n", "n/a");
        printf("  Throughput (burst, payload) : %10s\n", "n/a");
    }
}

#endif /* UDP_RX_TIMING_H */
