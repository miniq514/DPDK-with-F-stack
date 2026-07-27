/*
 * sender/tcp_stream_send.c
 *
 * 内核态 IPv6 TCP 打流客户端（替代 TCP 场景下的 pktgen）。
 * 按帧发送：8B 头(seq+len) + payload 体；结束打印 achieved_mbps / send_block 等自证统计。
 *
 * 用法:
 *   ./build/sender/tcp_stream_send --to-8004 --count 100 --rate-mbps 3000 --raw-payload
 */

#define _GNU_SOURCE

#include "sender_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    if (!s || !*s)
        return -1;
    v = strtoul(s, &end, 10);
    if (end && *end)
        return -1;
    if (v > UINT32_MAX)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

struct send_cfg {
    const char *dst_ip6;
    uint16_t    dst_port;
    uint32_t    count;
    uint32_t    payload_body; /* 数据区长度；raw 帧总长 = 8 + body */
    uint32_t    rate_mbps;    /* 0 = 不限速尽力发 */
    int         raw_payload;  /* 1=发送完整 8+body 帧作为 raw 块 */
    int         nodelay;
};

struct send_stats {
    uint64_t tx_frames;
    uint64_t tx_bytes;
    uint64_t send_block_spins;
    uint64_t send_block_ns;
    uint64_t connect_ns;
    uint64_t burst_ns;
};

static ssize_t send_all(int fd, const uint8_t *buf, size_t len,
                        struct send_stats *st)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
            uint64_t t0 = mono_ns();
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            st->send_block_spins++;
            (void)poll(&pfd, 1, 1000);
            st->send_block_ns += mono_ns() - t0;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return (ssize_t)len;
}

static void fill_frame(uint8_t *frame, uint32_t seq, uint32_t body_len, int raw)
{
    uint32_t total = SEND_PAYLOAD_HDR_SIZE + body_len;
    uint32_t i;

    (void)raw;
    memcpy(frame, &seq, 4);
    memcpy(frame + 4, &body_len, 4);
    for (i = 0; i < body_len; i++)
        frame[SEND_PAYLOAD_HDR_SIZE + i] = (uint8_t)((seq + i) & 0xff);
    (void)total;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--to-8004|--to-8007] [--count N] [--rate-mbps M] "
            "[--payload-size B] [--dst-port P] [--raw-payload]\n",
            prog);
}

int main(int argc, char **argv)
{
    struct send_cfg cfg = {
        .dst_ip6 = SEND_TARGET_8004_IP6,
        .dst_port = SEND_UDP_DST_PORT,
        .count = 100,
        .payload_body = SEND_PAYLOAD_SIZE,
        .rate_mbps = 0,
        .raw_payload = 1,
        .nodelay = 1,
    };
    struct send_stats st;
    int fd = -1;
    int on = 1;
    int opt;
    uint8_t *frame = NULL;
    uint32_t frame_len;
    uint32_t seq;
    uint64_t t_connect0, t_burst0, t_burst1;
    double sec, achieved_mbps, target_mbps;
    int sndbuf = 16 * 1024 * 1024;
    double ns_per_frame = 0.0;
    uint64_t next_deadline_ns = 0;

    static struct option long_opts[] = {
        {"to-8004",       no_argument,       NULL, '4'},
        {"to-8007",       no_argument,       NULL, '7'},
        {"count",         required_argument, NULL, 'c'},
        {"rate-mbps",     required_argument, NULL, 'r'},
        {"payload-size",  required_argument, NULL, 'p'},
        {"dst-port",      required_argument, NULL, 'd'},
        {"dst-ip",        required_argument, NULL, 'i'},
        {"raw-payload",   no_argument,       NULL, 'R'},
        {"no-raw-payload",no_argument,       NULL, 'N'},
        {"help",          no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "47c:r:p:d:i:RNh", long_opts, NULL)) != -1) {
        switch (opt) {
        case '4':
            cfg.dst_ip6 = SEND_TARGET_8004_IP6;
            break;
        case '7':
            cfg.dst_ip6 = SEND_TARGET_8007_IP6;
            break;
        case 'c':
            if (parse_u32(optarg, &cfg.count) < 0)
                goto bad;
            break;
        case 'r':
            if (parse_u32(optarg, &cfg.rate_mbps) < 0)
                goto bad;
            break;
        case 'p':
            if (parse_u32(optarg, &cfg.payload_body) < 0)
                goto bad;
            break;
        case 'd': {
            uint32_t p;
            if (parse_u32(optarg, &p) < 0 || p > 65535)
                goto bad;
            cfg.dst_port = (uint16_t)p;
            break;
        }
        case 'i':
            cfg.dst_ip6 = optarg;
            break;
        case 'R':
            cfg.raw_payload = 1;
            break;
        case 'N':
            cfg.raw_payload = 0;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            goto bad;
        }
    }

    frame_len = SEND_PAYLOAD_HDR_SIZE + cfg.payload_body;
    frame = malloc(frame_len);
    if (!frame) {
        perror("malloc");
        return 1;
    }

    memset(&st, 0, sizeof(st));

    fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        free(frame);
        return 1;
    }

    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    if (cfg.nodelay)
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(cfg.dst_port);
    if (inet_pton(AF_INET6, cfg.dst_ip6, &addr.sin6_addr) != 1) {
        fprintf(stderr, "[ERR] bad dst ip: %s\n", cfg.dst_ip6);
        close(fd);
        free(frame);
        return 1;
    }

    fprintf(stderr, "[*] TCP connect to [%s]:%u count=%u frame=%uB rate_mbps=%u raw=%d\n",
            cfg.dst_ip6, cfg.dst_port, cfg.count, frame_len, cfg.rate_mbps,
            cfg.raw_payload);

    /* 阻塞 connect，确保握手完成后再发 */
    t_connect0 = mono_ns();
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        free(frame);
        return 1;
    }
    st.connect_ns = mono_ns() - t_connect0;
    fprintf(stderr, "[*] connected in %.3f ms\n",
            (double)st.connect_ns / 1e6);

    /* 发送阶段非阻塞，便于统计 sndbuf 满导致的阻塞 */
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* close 时短暂等待；对端 _exit 时可能收不到 FIN-ACK，避免无限挂死 */
    {
        struct linger lg = { .l_onoff = 1, .l_linger = 2 };
        (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    }

    if (cfg.rate_mbps > 0) {
        /* payload 比特率口径：frame_len * 8 * rate */
        ns_per_frame = ((double)frame_len * 8.0 * 1e9) /
                       ((double)cfg.rate_mbps * 1e6);
    }

    t_burst0 = mono_ns();
    next_deadline_ns = t_burst0;

    for (seq = 0; seq < cfg.count; seq++) {
        fill_frame(frame, seq, cfg.payload_body, cfg.raw_payload);
        if (send_all(fd, frame, frame_len, &st) < 0) {
            fprintf(stderr, "[ERR] send failed at seq=%u: %s\n",
                    seq, strerror(errno));
            break;
        }
        st.tx_frames++;
        st.tx_bytes += frame_len;

        if (cfg.rate_mbps > 0) {
            next_deadline_ns += (uint64_t)ns_per_frame;
            for (;;) {
                uint64_t now = mono_ns();
                if (now >= next_deadline_ns)
                    break;
                uint64_t remain = next_deadline_ns - now;
                if (remain > 200000ull) {
                    struct timespec ts = {
                        .tv_sec = 0,
                        .tv_nsec = (long)(remain > 5000000ull ? 5000000ull : remain)
                    };
                    nanosleep(&ts, NULL);
                } else {
                    /* 忙等短间隙，减少限速抖动 */
                }
            }
        }
    }

    t_burst1 = mono_ns();
    st.burst_ns = t_burst1 - t_burst0;
    sec = (double)st.burst_ns / 1e9;
    if (sec < 1e-9)
        sec = 1e-9;
    achieved_mbps = ((double)st.tx_bytes * 8.0) / (sec * 1e6);
    target_mbps = (double)cfg.rate_mbps;

    /* 半关闭写端；依赖 SO_LINGER 在 close 时刷未确认数据（最多 30s） */
    (void)shutdown(fd, SHUT_WR);
    close(fd);
    free(frame);

    printf("\n");
    printf("========== Kernel TCP TX Statistics ==========\n");
    printf("  tx_frames          : %12" PRIu64 "\n", st.tx_frames);
    printf("  tx_bytes           : %12" PRIu64 "\n", st.tx_bytes);
    printf("  connect_ms         : %12.3f\n", (double)st.connect_ns / 1e6);
    printf("  burst_sec          : %12.6f\n", sec);
    printf("  target_mbps        : %12.1f\n", target_mbps);
    printf("  achieved_mbps      : %12.1f\n", achieved_mbps);
    printf("  send_block_spins   : %12" PRIu64 "\n", st.send_block_spins);
    printf("  send_block_ms      : %12.3f\n", (double)st.send_block_ns / 1e6);
    if (cfg.rate_mbps > 0) {
        double ratio = achieved_mbps / target_mbps;
        const char *note;
        if (ratio >= 0.95)
            note = "hit_target";
        else if (st.send_block_spins > st.tx_frames / 100)
            note = "sender_blocked_likely_recv_window";
        else
            note = "sender_limited";
        printf("  achieved/target    : %12.3f  (%s)\n", ratio, note);
    }
    printf("==============================================\n");
    fflush(stdout);

    return st.tx_frames == cfg.count ? 0 : 2;

bad:
    usage(argv[0]);
    return 1;
}
