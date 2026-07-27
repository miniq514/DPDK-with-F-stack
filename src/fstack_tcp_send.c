/*
 * src/fstack_tcp_send.c
 *
 * F-Stack TCP 打流客户端（发端）：ff_connect + ff_write，用于测收端真实上限。
 *
 * 运行示例（应用参数在 "--" 之后）：
 *   sudo ./build/fstack_tcp_send -c configs/config.send.dac.ini -- \
 *       --to-8004 --count 100000 --rate-mbps 0 --raw-payload
 */

#define _GNU_SOURCE

#include "sender_config.h"
#include "demo_udp_config.h"

#include "ff_api.h"
#include "ff_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum send_state {
    ST_INIT = 0,
    ST_CONNECTING,
    ST_SENDING,
    ST_DRAINING,
    ST_LINGER,
    ST_IDLE_PRIMARY,
    ST_DONE
};

struct app_cfg {
    const char *dst_ip6;
    const char *bind_ip6;
    uint16_t    dst_port;
    uint16_t    src_port;    /* 0=系统分配；多进程建议固定以免握手丢归属 */
    uint32_t    count;
    uint32_t    payload_body;
    uint32_t    rate_mbps;
    int         raw_payload;
    uint32_t    worker_id;   /* 多进程时本 worker 编号 */
    uint32_t    workers;     /* 总 worker 数（仅日志） */
    uint32_t    connect_delay_ms; /* 启动后延迟再 connect，便于 MP 全员就绪 */
    uint32_t    linger_sec;      /* 发完后主进程保活，避免 secondary 掉 primary */
    int         idle_primary;    /* 1=不连接，仅保活供 secondary 使用 */
};

struct tx_stats {
    uint64_t tx_frames;
    uint64_t tx_bytes;
    uint64_t write_block_spins;
    uint64_t write_block_ns;
    uint64_t connect_ns;
    uint64_t burst_ns;
};

static volatile sig_atomic_t g_running = 1;
static int g_sockfd = -1;
static enum send_state g_state = ST_INIT;
static struct app_cfg g_cfg;
static struct tx_stats g_st;
static uint8_t *g_frame;          /* 兼容：单帧指针（指向 blob 当前帧，可空） */
static uint32_t g_frame_len;
static uint8_t *g_blob;           /* 预生成的全部应用层帧（count * frame_len） */
static size_t   g_blob_len;
static size_t   g_blob_off;       /* 已成功写出的字节偏移 */
static uint32_t g_seq;
static uint64_t g_t_connect0;
static uint64_t g_t_burst0;
static uint64_t g_next_deadline_ns;
static double g_ns_per_frame;
static uint64_t g_t_drain0;
static int g_shutdown_done;
static int g_stats_emitted;
static uint64_t g_t_init0;
static uint64_t g_t_linger0;
static int g_lingering;

/* 单次 ff_write 尽量大块，减少 API 开销（仍按已预生成的连续 blob） */
#define SEND_WRITE_BURST  (64u * 1024u)

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void status_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !*s)
        return -1;
    v = strtoul(s, &end, 10);
    if ((end && *end) || v > UINT32_MAX)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

/* 启动前预生成全部应用层帧（类比 pktgen 预生成 pcap；TCP 头仍由栈填） */
static int build_payload_blob(void)
{
    uint32_t i;
    uint64_t need;
    uint8_t *p;

    need = (uint64_t)g_cfg.count * (uint64_t)g_frame_len;
    if (need == 0 || need > (uint64_t)SIZE_MAX) {
        status_msg("[ERR] bad blob size count=%u frame=%u", g_cfg.count, g_frame_len);
        return -1;
    }

    status_msg("[*] prebuilding payload blob: %u frames × %u B = %.2f MiB ...",
               g_cfg.count, g_frame_len, (double)need / (1024.0 * 1024.0));

    g_blob = malloc((size_t)need);
    if (!g_blob) {
        perror("malloc payload blob");
        return -1;
    }
    g_blob_len = (size_t)need;
    g_blob_off = 0;

    p = g_blob;
    for (i = 0; i < g_cfg.count; i++) {
        uint32_t body = g_cfg.payload_body;
        memset(p, 0xA5, g_frame_len);
        memcpy(p, &i, 4);
        memcpy(p + 4, &body, 4);
        p += g_frame_len;
    }

    status_msg("[*] payload blob ready (send will only ff_write from this buffer)");
    return 0;
}

/* 从预生成 blob 尽量写出；返回 1=有进展, 0=EAGAIN, -1=错 */
static int write_from_blob(void)
{
    size_t remain = g_blob_len - g_blob_off;
    size_t want;
    size_t done = 0;

    if (remain == 0)
        return 1;

    want = remain;
    if (want > SEND_WRITE_BURST)
        want = SEND_WRITE_BURST;

    while (done < want) {
        ssize_t n = ff_write(g_sockfd, g_blob + g_blob_off + done, want - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
            uint64_t t0 = mono_ns();
            g_st.write_block_spins++;
            g_st.write_block_ns += mono_ns() - t0;
            if (done == 0)
                return 0;
            break;
        }
        if (n < 0 && errno == EINTR)
            continue;
        status_msg("[ERR] ff_write off=%zu: %s", g_blob_off + done, strerror(errno));
        return -1;
    }

    g_blob_off += done;
    g_st.tx_bytes += done;
    g_st.tx_frames = (uint64_t)(g_blob_off / g_frame_len);
    g_seq = (uint32_t)g_st.tx_frames;
    return 1;
}

static void emit_stats_and_exit(int code)
{
    double sec, achieved, target;

    if (g_stats_emitted)
        _exit(code);
    g_stats_emitted = 1;

    if (g_st.burst_ns == 0 && g_t_burst0)
        g_st.burst_ns = mono_ns() - g_t_burst0;
    sec = (double)g_st.burst_ns / 1e9;
    if (sec < 1e-9)
        sec = 1e-9;
    achieved = ((double)g_st.tx_bytes * 8.0) / (sec * 1e6);
    target = (double)g_cfg.rate_mbps;

    printf("\n");
    printf("========== F-Stack TCP TX Statistics ==========\n");
    printf("  worker             : %u / %u\n",
           g_cfg.worker_id, g_cfg.workers ? g_cfg.workers : 1);
    printf("  tx_frames          : %12" PRIu64 "\n", g_st.tx_frames);
    printf("  tx_bytes           : %12" PRIu64 "\n", g_st.tx_bytes);
    printf("  connect_ms         : %12.3f\n", (double)g_st.connect_ns / 1e6);
    printf("  burst_sec          : %12.6f\n", sec);
    printf("  target_mbps        : %12.1f\n", target);
    printf("  achieved_mbps      : %12.1f\n", achieved);
    printf("  write_block_spins  : %12" PRIu64 "\n", g_st.write_block_spins);
    printf("  write_block_ms     : %12.3f\n", (double)g_st.write_block_ns / 1e6);
    if (g_cfg.rate_mbps > 0) {
        double ratio = achieved / target;
        const char *note = (ratio >= 0.95) ? "hit_target" :
            (g_st.write_block_spins > g_st.tx_frames / 100 ?
             "blocked_likely_peer_window" : "sender_limited");
        printf("  achieved/target    : %12.3f  (%s)\n", ratio, note);
    }
    printf("==============================================\n");
    fflush(stdout);
    _exit(code);
}

static int split_argv_at_dd(int argc, char **argv,
                            int *ff_argc, char ***ff_argv,
                            int *app_argc, char ***app_argv)
{
    int sep = -1;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            sep = i;
            break;
        }
    }
    if (sep < 0) {
        *ff_argc = argc;
        *ff_argv = argv;
        *app_argc = 0;
        *app_argv = &argv[argc];
        return 0;
    }
    *ff_argc = sep;
    *ff_argv = argv;
    *app_argc = argc - sep - 1;
    *app_argv = argv + sep + 1;
    return 0;
}

static void parse_app_args(int argc, char **argv, struct app_cfg *cfg)
{
    cfg->dst_ip6 = SEND_TARGET_8004_IP6;
    cfg->bind_ip6 = SEND_SRC_8007_IP6;
    cfg->dst_port = SEND_UDP_DST_PORT;
    cfg->src_port = 0;
    cfg->count = 100;
    cfg->payload_body = SEND_PAYLOAD_SIZE;
    cfg->rate_mbps = 0;
    cfg->raw_payload = 1;
    cfg->worker_id = 0;
    cfg->workers = 1;
    cfg->connect_delay_ms = 0;
    cfg->linger_sec = 0;
    cfg->idle_primary = 0;

    if (argc <= 0)
        return;

    static const char prog[] = "fstack_tcp_send";
    char *optv[256];
    int optc = argc + 1;
    if (optc > (int)(sizeof(optv) / sizeof(optv[0])))
        goto bad;
    optv[0] = (char *)prog;
    for (int i = 0; i < argc; i++)
        optv[i + 1] = argv[i];

    static struct option long_opts[] = {
        {"to-8004",        no_argument,       NULL, '4'},
        {"to-8007",        no_argument,       NULL, '7'},
        {"count",          required_argument, NULL, 'c'},
        {"rate-mbps",      required_argument, NULL, 'r'},
        {"payload-size",   required_argument, NULL, 'p'},
        {"dst-port",       required_argument, NULL, 'd'},
        {"src-port",       required_argument, NULL, 'S'},
        {"dst-ip",         required_argument, NULL, 'i'},
        {"bind-ip",        required_argument, NULL, 'b'},
        {"raw-payload",    no_argument,       NULL, 'R'},
        {"no-raw-payload", no_argument,       NULL, 'N'},
        {"worker-id",      required_argument, NULL, 'W'},
        {"workers",        required_argument, NULL, 'T'},
        {"connect-delay-ms", required_argument, NULL, 'D'},
        {"linger-sec",     required_argument, NULL, 'L'},
        {"idle-primary",   no_argument,       NULL, 'I'},
        {"help",           no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(optc, optv, "47c:r:p:d:S:i:b:RNhW:T:D:L:I", long_opts, NULL)) != -1) {
        switch (opt) {
        case '4':
            cfg->dst_ip6 = SEND_TARGET_8004_IP6;
            cfg->bind_ip6 = SEND_SRC_8007_IP6;
            break;
        case '7':
            cfg->dst_ip6 = SEND_TARGET_8007_IP6;
            cfg->bind_ip6 = SEND_SRC_8004_IP6;
            break;
        case 'c':
            if (parse_u32(optarg, &cfg->count) < 0)
                goto bad;
            break;
        case 'r':
            if (parse_u32(optarg, &cfg->rate_mbps) < 0)
                goto bad;
            break;
        case 'p':
            if (parse_u32(optarg, &cfg->payload_body) < 0)
                goto bad;
            break;
        case 'd': {
            uint32_t p;
            if (parse_u32(optarg, &p) < 0 || p > 65535)
                goto bad;
            cfg->dst_port = (uint16_t)p;
            break;
        }
        case 'S': {
            uint32_t p;
            if (parse_u32(optarg, &p) < 0 || p > 65535)
                goto bad;
            cfg->src_port = (uint16_t)p;
            break;
        }
        case 'i':
            cfg->dst_ip6 = optarg;
            break;
        case 'b':
            cfg->bind_ip6 = optarg;
            break;
        case 'R':
            cfg->raw_payload = 1;
            break;
        case 'N':
            cfg->raw_payload = 0;
            break;
        case 'W':
            if (parse_u32(optarg, &cfg->worker_id) < 0)
                goto bad;
            break;
        case 'T':
            if (parse_u32(optarg, &cfg->workers) < 0 || cfg->workers == 0)
                goto bad;
            break;
        case 'D':
            if (parse_u32(optarg, &cfg->connect_delay_ms) < 0)
                goto bad;
            break;
        case 'L':
            if (parse_u32(optarg, &cfg->linger_sec) < 0)
                goto bad;
            break;
        case 'I':
            cfg->idle_primary = 1;
            break;
        case 'h':
            fprintf(stderr,
                    "Usage: %s -c config.ini -- [--to-8004] [--count N] "
                    "[--rate-mbps M] [--payload-size B] "
                    "[--worker-id I] [--workers N]\n", prog);
            exit(0);
        default:
            goto bad;
        }
    }
    (void)cfg->raw_payload;
    return;
bad:
    fprintf(stderr, "[ERR] bad app args\n");
    exit(EXIT_FAILURE);
}

static int sock_setup_and_connect(void)
{
    struct sockaddr_in6 local, peer;
    int on = 1;
    int snd = 16 * 1024 * 1024;
    int fd;

    fd = ff_socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        status_msg("[ERR] ff_socket: %s", strerror(errno));
        return -1;
    }
    if (ff_ioctl(fd, FIONBIO, &on) < 0) {
        status_msg("[ERR] FIONBIO: %s", strerror(errno));
        ff_close(fd);
        return -1;
    }
    (void)ff_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    (void)ff_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    memset(&local, 0, sizeof(local));
    local.sin6_family = AF_INET6;
    local.sin6_port = htons(g_cfg.src_port);
    if (inet_pton(AF_INET6, g_cfg.bind_ip6, &local.sin6_addr) != 1) {
        status_msg("[ERR] bad bind-ip %s", g_cfg.bind_ip6);
        ff_close(fd);
        return -1;
    }
    if (ff_bind(fd, (const struct linux_sockaddr *)&local, sizeof(local)) < 0) {
        status_msg("[!] ff_bind %s sport=%u failed (%s), continue without bind",
                   g_cfg.bind_ip6, g_cfg.src_port, strerror(errno));
    } else if (g_cfg.src_port) {
        status_msg("[*] bound src_port=%u", g_cfg.src_port);
    }

    memset(&peer, 0, sizeof(peer));
    peer.sin6_family = AF_INET6;
    peer.sin6_port = htons(g_cfg.dst_port);
    if (inet_pton(AF_INET6, g_cfg.dst_ip6, &peer.sin6_addr) != 1) {
        status_msg("[ERR] bad dst-ip %s", g_cfg.dst_ip6);
        ff_close(fd);
        return -1;
    }

    g_t_connect0 = mono_ns();
    if (ff_connect(fd, (const struct linux_sockaddr *)&peer, sizeof(peer)) < 0) {
        if (errno != EINPROGRESS && errno != EALREADY) {
            status_msg("[ERR] ff_connect: %s", strerror(errno));
            ff_close(fd);
            return -1;
        }
        g_state = ST_CONNECTING;
        status_msg("[*] ff_connect in progress → [%s]:%u",
                   g_cfg.dst_ip6, g_cfg.dst_port);
    } else {
        g_st.connect_ns = mono_ns() - g_t_connect0;
        g_state = ST_SENDING;
        g_t_burst0 = mono_ns();
        g_next_deadline_ns = g_t_burst0;
        status_msg("[*] connected immediately in %.3f ms",
                   (double)g_st.connect_ns / 1e6);
    }

    g_sockfd = fd;
    return 0;
}

static int check_connected(void)
{
    int err = 0;
    socklen_t len = sizeof(err);
    struct sockaddr_storage peer;
    socklen_t plen = sizeof(peer);

    if (ff_getsockopt(g_sockfd, SOL_SOCKET, SO_ERROR, &err, &len) == 0) {
        if (err != 0) {
            status_msg("[ERR] connect SO_ERROR: %s", strerror(err));
            return -1;
        }
    }

    /* getpeername 成功则已建立 */
    if (ff_getpeername(g_sockfd, (struct linux_sockaddr *)&peer, &plen) == 0) {
        g_st.connect_ns = mono_ns() - g_t_connect0;
        g_state = ST_SENDING;
        g_t_burst0 = mono_ns();
        g_next_deadline_ns = g_t_burst0;
        status_msg("[*] connected in %.3f ms", (double)g_st.connect_ns / 1e6);
        return 1;
    }

    /* 试写探测：部分栈在可写前 getpeername 仍失败 */
    {
        uint8_t b = 0;
        ssize_t n = ff_write(g_sockfd, &b, 0);
        (void)n;
        if (ff_getpeername(g_sockfd, (struct linux_sockaddr *)&peer, &plen) == 0) {
            g_st.connect_ns = mono_ns() - g_t_connect0;
            g_state = ST_SENDING;
            g_t_burst0 = mono_ns();
            g_next_deadline_ns = g_t_burst0;
            status_msg("[*] connected in %.3f ms", (double)g_st.connect_ns / 1e6);
            return 1;
        }
    }
    return 0;
}

static int loop_cb_drain(void);

static int loop_cb(void *arg)
{
    int i;
    (void)arg;

    if (!g_running) {
        g_st.burst_ns = g_t_burst0 ? (mono_ns() - g_t_burst0) : 0;
        emit_stats_and_exit(1);
    }

    if (g_state == ST_INIT) {
        if (g_cfg.idle_primary) {
            g_t_linger0 = mono_ns();
            g_state = ST_IDLE_PRIMARY;
            status_msg("[*] idle-primary: keep F-Stack primary alive (no TCP send)");
            return 0;
        }
        if (g_cfg.connect_delay_ms > 0) {
            if (mono_ns() - g_t_init0 <
                (uint64_t)g_cfg.connect_delay_ms * 1000000ull)
                return 0;
        }
        if (sock_setup_and_connect() < 0)
            emit_stats_and_exit(1);
        return 0;
    }

    if (g_state == ST_IDLE_PRIMARY) {
        uint32_t hold = g_cfg.linger_sec ? g_cfg.linger_sec : 120;
        if (!g_running ||
            mono_ns() - g_t_linger0 >= (uint64_t)hold * 1000000000ull) {
            status_msg("[*] idle-primary exit");
            _exit(0);
        }
        return 0;
    }

    if (g_state == ST_CONNECTING) {
        int r = check_connected();
        if (r < 0)
            emit_stats_and_exit(1);
        /* 连接超时 15s */
        if (g_state == ST_CONNECTING &&
            mono_ns() - g_t_connect0 > 15ull * 1000000000ull) {
            status_msg("[ERR] connect timeout 15s");
            emit_stats_and_exit(1);
        }
        return 0;
    }

    if (g_state == ST_DRAINING)
        return loop_cb_drain();

    if (g_state == ST_LINGER) {
        if (mono_ns() - g_t_linger0 >= (uint64_t)g_cfg.linger_sec * 1000000000ull) {
            status_msg("[*] linger done, exit");
            _exit(0);
        }
        return 0;
    }

    if (g_state != ST_SENDING)
        return 0;

    /* 每轮尽量多写预生成 blob；受窗口限制时 EAGAIN 退出本轮 */
    for (i = 0; i < 64 && g_blob_off < g_blob_len && g_running; i++) {
        if (g_cfg.rate_mbps > 0) {
            uint64_t now = mono_ns();
            if (now < g_next_deadline_ns)
                break;
        }

        int wr = write_from_blob();
        if (wr < 0) {
            g_st.burst_ns = mono_ns() - g_t_burst0;
            emit_stats_and_exit(1);
        }
        if (wr == 0)
            break; /* EAGAIN */

        if (g_cfg.rate_mbps > 0) {
            /* 按本轮新写出的完整帧数推进限速（简化：每成功一次按 burst/frame 估） */
            g_next_deadline_ns += (uint64_t)g_ns_per_frame *
                (SEND_WRITE_BURST / g_frame_len);
            if (g_next_deadline_ns < mono_ns())
                g_next_deadline_ns = mono_ns();
        }
    }

    if (g_blob_off >= g_blob_len) {
        g_st.burst_ns = mono_ns() - g_t_burst0;
        status_msg("[*] frames queued=%" PRIu64 ", draining TCP TX...",
                   g_st.tx_frames);
        g_state = ST_DRAINING;
        g_t_drain0 = mono_ns();
        g_shutdown_done = 0;
    }
    return 0;
}

static void finish_or_linger(int code)
{
    if (g_cfg.linger_sec > 0 && !g_lingering) {
        g_lingering = 1;
        g_t_linger0 = mono_ns();
        g_state = ST_LINGER;
        status_msg("[*] linger %us (keep primary alive for secondaries)",
                   g_cfg.linger_sec);
        /* 先打统计，但进程不退出 */
        if (!g_stats_emitted) {
            double sec, achieved, target;
            g_stats_emitted = 1;
            if (g_st.burst_ns == 0 && g_t_burst0)
                g_st.burst_ns = mono_ns() - g_t_burst0;
            sec = (double)g_st.burst_ns / 1e9;
            if (sec < 1e-9)
                sec = 1e-9;
            achieved = ((double)g_st.tx_bytes * 8.0) / (sec * 1e6);
            target = (double)g_cfg.rate_mbps;
            printf("\n");
            printf("========== F-Stack TCP TX Statistics ==========\n");
            printf("  worker             : %u / %u\n",
                   g_cfg.worker_id, g_cfg.workers ? g_cfg.workers : 1);
            printf("  tx_frames          : %12" PRIu64 "\n", g_st.tx_frames);
            printf("  tx_bytes           : %12" PRIu64 "\n", g_st.tx_bytes);
            printf("  connect_ms         : %12.3f\n", (double)g_st.connect_ns / 1e6);
            printf("  burst_sec          : %12.6f\n", sec);
            printf("  target_mbps        : %12.1f\n", target);
            printf("  achieved_mbps      : %12.1f\n", achieved);
            printf("  write_block_spins  : %12" PRIu64 "\n", g_st.write_block_spins);
            printf("  write_block_ms     : %12.3f\n", (double)g_st.write_block_ns / 1e6);
            printf("==============================================\n");
            fflush(stdout);
        }
        (void)code;
        return;
    }
    emit_stats_and_exit(code);
}

static int loop_cb_drain(void)
{
    char tmp[64];
    ssize_t n;

    if (!g_shutdown_done) {
        if (ff_shutdown(g_sockfd, SHUT_WR) < 0 &&
            errno != ENOTCONN && errno != EINVAL) {
            status_msg("[!] ff_shutdown: %s", strerror(errno));
        }
        g_shutdown_done = 1;
    }

    n = ff_read(g_sockfd, tmp, sizeof(tmp));
    if (n == 0) {
        status_msg("[*] peer closed after drain");
        ff_close(g_sockfd);
        g_sockfd = -1;
        finish_or_linger(0);
        return 0;
    }

    if (mono_ns() - g_t_drain0 > 5ull * 1000000000ull) {
        status_msg("[*] drain timeout 5s, exit");
        if (g_sockfd >= 0) {
            ff_close(g_sockfd);
            g_sockfd = -1;
        }
        finish_or_linger(0);
        return 0;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int ff_argc, app_argc;
    char **ff_argv, **app_argv;
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    setlinebuf(stdout);

    split_argv_at_dd(argc, argv, &ff_argc, &ff_argv, &app_argc, &app_argv);
    parse_app_args(app_argc, app_argv, &g_cfg);

    g_frame_len = SEND_PAYLOAD_HDR_SIZE + g_cfg.payload_body;
    g_frame = NULL;

    if (g_cfg.rate_mbps > 0) {
        g_ns_per_frame = ((double)g_frame_len * 8.0 * 1e9) /
                         ((double)g_cfg.rate_mbps * 1e6);
    }

    printf("[*] F-Stack TCP TX: worker=%u/%u bind=%s → [%s]:%u count=%u "
           "frame=%uB rate_mbps=%u\n",
           g_cfg.worker_id, g_cfg.workers ? g_cfg.workers : 1,
           g_cfg.bind_ip6, g_cfg.dst_ip6, g_cfg.dst_port, g_cfg.count,
           g_frame_len, g_cfg.rate_mbps);

    if (!g_cfg.idle_primary) {
        if (build_payload_blob() < 0)
            return 1;
    }

    if (ff_init(ff_argc, ff_argv) < 0) {
        fprintf(stderr, "ff_init failed\n");
        return 1;
    }
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    g_t_init0 = mono_ns();

    ff_run(loop_cb, NULL);
    emit_stats_and_exit(1);
    return 1;
}
