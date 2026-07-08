/* recvmmsg 需要 _GNU_SOURCE */
#define _GNU_SOURCE

/*
 * io-uring/linux_udp_recv.c
 *
 * Linux UDP 基线接收程序：内核态 UDP socket 收包 + 共用 io_uring writer 落盘。
 * 作为 DPDK 路径的性能对照组，保证写盘逻辑完全一致。
 *
 * 用法:
 *   ./linux_udp_recv [--bind-ip IP] [--port PORT] [--output PATH]
 *                    [--expected-pkts N] [--duration SEC]
 *
 * 默认: 监听 10.10.10.1:50000，输出到 Test/udp_linux_output.bin
 *
 * 收包方式：recvmmsg 批量接收 → 剥离 8 字节 payload header → udp_writer_submit
 * 信号处理：SIGINT/SIGTERM 优雅退出
 */

#include "../include/demo_udp_config.h"
#include "../include/udp_payload.h"
#include "../include/udp_rx_timing.h"
#include "udp_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

/* ============ 全局状态 ============ */
static volatile sig_atomic_t g_running = 1;

static void signal_handler(int signo)
{
    (void)signo;
    g_running = 0;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int rx_should_idle_exit(uint32_t expected_pkts, uint64_t matched,
                               const struct udp_rx_burst_timing *burst)
{
    /*
     * 仅在收到首包之后启用：发端未启动前可一直等，不设超时。
     * 有进包后若连续 N 秒无新 matched，视为发端已停或丢包收尾。
     */
    if (expected_pkts == 0 || UDP_RX_IDLE_EXIT_SEC == 0)
        return 0;
    if (!burst->active || matched == 0 || matched >= expected_pkts)
        return 0;

    uint64_t now_ns = udp_rx_monotonic_ns();

    return now_ns - burst->last_ns >=
           (uint64_t)UDP_RX_IDLE_EXIT_SEC * 1000000000ull;
}

/* 状态信息走 stderr 并立即 flush，与 DPDK 收端一致 */
static void status_msg(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ============ 应用参数 ============ */
struct app_config {
    const char *bind_ip;
    uint16_t    port;
    const char *output_path;
    uint32_t    expected_pkts;   /* 0 = 无限 */
    uint32_t    duration_sec;    /* 0 = 无限 */
};

static int parse_u16_arg(const char *s, uint16_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);

    if (!s || !*s || (end && *end != '\0') || v > UINT16_MAX)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

static int parse_u32_arg(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);

    if (!s || !*s || (end && *end != '\0') || v > UINT32_MAX)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static void parse_args(int argc, char **argv, struct app_config *cfg)
{
    cfg->bind_ip       = UDP_LOCAL_IP;
    cfg->port          = UDP_DST_PORT;
    cfg->output_path   = UDP_LINUX_OUTPUT;
    cfg->expected_pkts = 0;
    cfg->duration_sec  = 0;

    static struct option long_opts[] = {
        {"bind-ip",       required_argument, NULL, 'b'},
        {"port",          required_argument, NULL, 'p'},
        {"output",        required_argument, NULL, 'o'},
        {"expected-pkts", required_argument, NULL, 'e'},
        {"duration",      required_argument, NULL, 'd'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:p:o:e:d:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'b': cfg->bind_ip = optarg; break;
        case 'p':
            if (parse_u16_arg(optarg, &cfg->port) < 0) {
                fprintf(stderr, "Invalid --port: %s\n", optarg);
                exit(1);
            }
            break;
        case 'o': cfg->output_path = optarg; break;
        case 'e':
            if (parse_u32_arg(optarg, &cfg->expected_pkts) < 0) {
                fprintf(stderr, "Invalid --expected-pkts: %s\n", optarg);
                exit(1);
            }
            break;
        case 'd':
            if (parse_u32_arg(optarg, &cfg->duration_sec) < 0) {
                fprintf(stderr, "Invalid --duration: %s\n", optarg);
                exit(1);
            }
            break;
        default:
            fprintf(stderr,
                "Usage: %s [--bind-ip IP] [--port PORT] [--output PATH]\n"
                "           [--expected-pkts N] [--duration SEC]\n", argv[0]);
            exit(1);
        }
    }
}

/* ============ 收包统计 ============ */
struct rx_stats {
    uint64_t total_rx;          /* 总收包数 */
    uint64_t total_rx_bytes;    /* 总收包字节（含 UDP 头） */
    uint64_t udp_matched;       /* 提取到数据区的包数 */
    uint64_t payload_bytes;     /* 写入 writer 的 payload 字节 */
    uint64_t too_short;         /* payload 太短，跳过的包 */
    uint64_t payload_errors;    /* payload 头校验失败 */
    uint64_t writer_errors;     /* 写盘失败 */
    struct udp_payload_validate_stats payload_validate;
};

static void print_stats(const struct rx_stats *s, uint32_t expected_pkts,
                        const struct udp_rx_burst_timing *burst,
                        double total_elapsed_sec,
                        const struct udp_writer_stats *ws)
{
    printf("\n");
    printf("========== Linux UDP RX Statistics =========\n");
    printf("  Total RX packets   : %12" PRIu64 "\n", s->total_rx);
    printf("  Total RX bytes     : %12" PRIu64 "  (%.2f MB)\n",
           s->total_rx_bytes, (double)s->total_rx_bytes / (1024.0 * 1024.0));
    printf("  Payload extracted  : %12" PRIu64 "\n", s->udp_matched);
    printf("  Payload bytes      : %12" PRIu64 "  (%.2f MB)\n",
           s->payload_bytes, (double)s->payload_bytes / (1024.0 * 1024.0));
    printf("  Too short          : %12" PRIu64 "\n", s->too_short);
    printf("  Payload errors     : %12" PRIu64 "\n", s->payload_errors);
    printf("  Seq gaps           : %12" PRIu64 "\n", s->payload_validate.seq_gaps);
    printf("  Seq reorder        : %12" PRIu64 "\n", s->payload_validate.seq_reorder);
    printf("  Length mismatch    : %12" PRIu64 "\n",
           s->payload_validate.length_mismatch);
    printf("  Writer errors      : %12" PRIu64 "\n", s->writer_errors);
    udp_rx_print_timing(expected_pkts, burst, s->total_rx, s->total_rx_bytes,
                        s->udp_matched, s->payload_bytes, total_elapsed_sec);
    if (ws)
        udp_writer_print_throughput(ws);
    printf("=============================================\n");
}

/* ============ 主逻辑 ============ */

/*
 * recvmmsg 批量接收缓冲区配置
 */
#define VLEN        32                          /* 一次 recvmmsg 最多收 32 包 */
#define RECV_BUF_SZ (65536)                     /* 每个 iov 的缓冲区大小 */

int main(int argc, char *argv[])
{
    install_signal_handlers();

    struct app_config cfg;
    parse_args(argc, argv, &cfg);

    printf("[*] Config: bind=%s:%u, output=%s, expected_pkts=%u, duration=%u\n",
           cfg.bind_ip, cfg.port, cfg.output_path,
           cfg.expected_pkts, cfg.duration_sec);

    /* 1. 创建 UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    /* 允许端口重用 */
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(sock);
        return 1;
    }

    /* 增大接收缓冲区（高速打流时减少内核丢包） */
    int rcvbuf = 16 * 1024 * 1024;  /* 16 MB */
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0)
        perror("setsockopt SO_RCVBUF");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    addr.sin_addr.s_addr = inet_addr(cfg.bind_ip);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }
    printf("[+] Listening on %s:%u ...\n", cfg.bind_ip, cfg.port);

    /* 2. 创建 udp_writer */
    struct udp_writer *writer = udp_writer_create(cfg.output_path, 0);
    if (!writer) {
        fprintf(stderr, "Failed to create udp_writer\n");
        close(sock);
        return 1;
    }

    /* 3. 准备 recvmmsg 批量接收（B11: 堆分配，避免大栈数组） */
    struct iovec iovecs[VLEN];
    struct mmsghdr msgs[VLEN];
    char *bufs[VLEN];

    for (int i = 0; i < VLEN; i++) {
        bufs[i] = malloc(RECV_BUF_SZ);
        if (!bufs[i]) {
            perror("malloc recv buffer");
            for (int j = 0; j < i; j++)
                free(bufs[j]);
            udp_writer_destroy(writer);
            close(sock);
            return 1;
        }
    }

    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < VLEN; i++) {
        iovecs[i].iov_base = bufs[i];
        iovecs[i].iov_len = RECV_BUF_SZ;
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    /* 4. 设置超时（用于检测退出条件和 duration 到期） */
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 5. 收包主循环 */
    struct rx_stats stats;
    memset(&stats, 0, sizeof(stats));

    struct timespec ts_start, ts_now;
    struct timespec ts_active_start;
    struct timespec last_wait_log;
    int timing_active = 0;   /* 首包到达后才开始吞吐计时 */
    int wait_log_init = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    uint32_t next_expected_seq = 0;
    struct udp_rx_burst_timing burst;
    int idle_stopped = 0;

    memset(&burst, 0, sizeof(burst));

    if (cfg.expected_pkts > 0) {
        status_msg("[*] READY — listening %s:%u, target %u packets",
                   cfg.bind_ip, cfg.port, cfg.expected_pkts);
        status_msg("[*] Blocking in recvmmsg(); send anytime, Ctrl+C to stop");
        if (UDP_RX_IDLE_EXIT_SEC > 0) {
            status_msg("[*] Idle exit: %u s without new pkt after first received "
                       "(unlimited wait before first pkt)",
                       (unsigned)UDP_RX_IDLE_EXIT_SEC);
        }
    }

    while (g_running) {
        /* 检查 duration 超时 */
        if (cfg.duration_sec > 0) {
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            double elapsed = (double)(ts_now.tv_sec - ts_start.tv_sec) +
                             (double)(ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;
            if (elapsed >= (double)cfg.duration_sec) {
                printf("[*] Duration %u s reached, stopping\n", cfg.duration_sec);
                break;
            }
        }

        int n = recvmmsg(sock, msgs, VLEN, MSG_WAITFORONE, NULL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (cfg.expected_pkts > 0) {
                    clock_gettime(CLOCK_MONOTONIC, &ts_now);
                    if (!wait_log_init) {
                        last_wait_log = ts_now;
                        wait_log_init = 1;
                    } else {
                        double since_log = (double)(ts_now.tv_sec - last_wait_log.tv_sec) +
                                           (double)(ts_now.tv_nsec - last_wait_log.tv_nsec) / 1e9;
                        if (since_log >= 5.0) {
                            status_msg("[*] Waiting... matched %lu/%u (Ctrl+C to stop)",
                                       (unsigned long)stats.udp_matched,
                                       cfg.expected_pkts);
                            last_wait_log = ts_now;
                        }
                    }
                    if (rx_should_idle_exit(cfg.expected_pkts, stats.udp_matched,
                                            &burst)) {
                        status_msg("[*] No progress for %u s, stopping incomplete at %lu/%u",
                                   (unsigned)UDP_RX_IDLE_EXIT_SEC,
                                   (unsigned long)stats.udp_matched,
                                   cfg.expected_pkts);
                        idle_stopped = 1;
                        break;
                    }
                }
                if (!g_running)
                    break;
                continue;   /* 超时，重新检查退出条件 */
            }
            if (errno == EINTR) {
                if (!g_running)
                    break;
                continue;   /* 可恢复信号，继续收包 */
            }
            perror("recvmmsg");
            break;
        }

        if (!timing_active) {
            clock_gettime(CLOCK_MONOTONIC, &ts_active_start);
            timing_active = 1;
        }

        for (int i = 0; i < n; i++) {
            uint32_t pkt_len = (uint32_t)msgs[i].msg_len;
            stats.total_rx++;
            stats.total_rx_bytes += pkt_len;

            /*
             * recvmmsg 拿到的是 UDP payload（内核已剥离 L2/L3/L4 头）。
             * payload 格式：[seq:4][length:4][data...]
             */
            if (pkt_len <= UDP_PAYLOAD_HDR_SIZE) {
                stats.too_short++;
                continue;
            }

            const uint8_t *payload = (const uint8_t *)bufs[i];
            const uint8_t *data_area = NULL;
            uint32_t data_len = 0;

            if (udp_payload_extract(payload, pkt_len, &next_expected_seq,
                                    &stats.payload_validate,
                                    &data_area, &data_len) < 0) {
                stats.payload_errors++;
                continue;
            }

            uint32_t submitted_seq = next_expected_seq - 1;

            stats.udp_matched++;
            stats.payload_bytes += data_len;

            if (udp_writer_submit(writer, data_area, data_len) < 0) {
                stats.writer_errors++;
            } else {
                udp_rx_burst_on_submit(&burst, submitted_seq);
            }

            /* 检查预期包数 */
            if (cfg.expected_pkts > 0 &&
                stats.udp_matched >= cfg.expected_pkts) {
                printf("[*] Reached expected %u packets, stopping\n",
                       cfg.expected_pkts);
                g_running = 0;
                break;
            }
        }
    }

    /* 6. 刷新写盘（B12: flush 计入端到端耗时） */
    if (udp_writer_flush(writer) < 0)
        stats.writer_errors++;

    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    double total_elapsed = 0.0;
    if (timing_active) {
        total_elapsed = (double)(ts_now.tv_sec - ts_active_start.tv_sec) +
                        (double)(ts_now.tv_nsec - ts_active_start.tv_nsec) / 1e9;
    }

    struct udp_writer_stats ws = udp_writer_get_stats(writer);
    print_stats(&stats, cfg.expected_pkts, &burst, total_elapsed, &ws);
    fflush(stdout);

    printf("[*] Writer: %lu B written to %s\n",
           (unsigned long)ws.bytes_written, cfg.output_path);
    fflush(stdout);

    /* 7. 清理 */
    udp_writer_destroy(writer);
    for (int i = 0; i < VLEN; i++)
        free(bufs[i]);
    close(sock);

    if (!g_running && !idle_stopped && cfg.expected_pkts > 0 &&
        stats.udp_matched < cfg.expected_pkts) {
        fprintf(stderr, "[!] Stopped by signal (Ctrl+C) at %lu/%u packets\n",
                (unsigned long)stats.udp_matched, cfg.expected_pkts);
    }

    if (cfg.expected_pkts > 0 && stats.udp_matched < cfg.expected_pkts) {
        fprintf(stderr,
                "[!] INCOMPLETE: matched %lu/%u packets — "
                "sender stopped early, kernel drops, idle timeout, or Ctrl+C\n",
                (unsigned long)stats.udp_matched, cfg.expected_pkts);
        return 1;
    }

    if (stats.writer_errors > 0 || stats.payload_errors > 0) {
        fprintf(stderr, "[!] Exiting with errors: writer=%" PRIu64
                ", payload=%" PRIu64 "\n",
                stats.writer_errors, stats.payload_errors);
        return 1;
    }
    return 0;
}
