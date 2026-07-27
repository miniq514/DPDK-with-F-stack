/*
 * src/fstack_tcp_recv.c
 *
 * F-Stack TCP 收包主程序（V2.0）：listen/accept/read + 流式成帧 → ring → io_uring 落盘。
 *
 * 运行示例：
 *   sudo ./build/fstack_tcp_recv -c configs/config.dac.ini -- \
 *       --output test/output/tcp_live.bin --dst-port 8888 --raw-payload \
 *       --expected-pkts 100
 */

#define _GNU_SOURCE

#include "fstack_udp_recv.h"
#include "demo_udp_config.h"
#include "udp_rx_timing.h"

#include "ff_api.h"
#include "ff_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_errno.h>
#include <rte_common.h>

/* 流式拼包缓冲：至少容纳一帧 + 一次 read 突发 */
#define TCP_STREAM_CAP   (UDP_RX_PAYLOAD_SIZE * 4u + 65536u)
#define TCP_READ_CHUNK   65536u
#define TCP_MAX_CONNS    64

struct tcp_conn {
    int      fd;
    size_t   stream_len;
    uint8_t *stream_buf;
};

static volatile sig_atomic_t g_running = 1;

static struct rte_ring    *g_rx_ring    = NULL;
static struct rte_mempool *g_desc_pool  = NULL;
static struct rte_mempool *g_cpbuf_pool = NULL;

static int g_listenfd = -1;
static int g_sock_ready = 0;
static int g_stats_emitted = 0;
static int g_nconns = 0;
static int g_ever_connected = 0;
static struct tcp_conn g_conns[TCP_MAX_CONNS];

/* 兼容旧单连接符号：仅用于心跳日志 */
static int g_connfd = -1;
static size_t g_stream_len = 0;

static void pipeline_emit_stats(const struct rx_loop_ctx *lctx,
                                const struct udp_writer_stats *ws)
{
    double total_sec;

    if (!lctx || !lctx->cfg || !lctx->stats || !lctx->burst || g_stats_emitted)
        return;

    g_stats_emitted = 1;
    total_sec = (double)(udp_rx_monotonic_ns() - lctx->pipeline_t0_ns) / 1e9;
    print_rx_stats(lctx->stats, lctx->cfg->expected_pkts, lctx->burst,
                   total_sec, ws);
    fflush(stdout);
}

/* 通知 writer 排空并 join，确保 flush/ftruncate/fsync 完成 */
static void pipeline_drain_writer(struct writer_ctx *wctx)
{
    if (!wctx)
        return;

    wctx->rx_done = 1;
    if (!wctx->tid_valid)
        return;

    if (pthread_join(wctx->tid, NULL) != 0)
        perror("pthread_join(writer)");
    wctx->tid_valid = 0;
}

static void pipeline_exit_now(struct rx_loop_ctx *lctx, const char *reason)
{
    const struct udp_writer_stats *ws = NULL;

    if (!lctx || !lctx->cfg || !lctx->stats)
        _exit(1);

    status_msg("[*] %s: matched %" PRIu64 "/%u frames (total_rx=%" PRIu64 ")",
               reason, lctx->stats->udp_matched, lctx->cfg->expected_pkts,
               lctx->stats->total_rx);

    /* 先等落盘收尾（含 O_DIRECT 尾零 ftruncate），再打统计并 _exit */
    pipeline_drain_writer(lctx->wctx);
    if (lctx->wctx) {
        lctx->stats->writer_errors += lctx->wctx->writer_errors;
        ws = &lctx->wctx->wstats;
    }
    pipeline_emit_stats(lctx, ws);

    if (lctx->cfg->expected_pkts > 0 &&
        lctx->stats->udp_matched < lctx->cfg->expected_pkts) {
        fprintf(stderr,
                "[!] INCOMPLETE: matched %" PRIu64 "/%u frames — "
                "total_rx=%" PRIu64 "\n",
                lctx->stats->udp_matched, lctx->cfg->expected_pkts,
                lctx->stats->total_rx);
        fflush(stderr);
        _exit(1);
    }

    _exit(0);
}

void signal_handler(int)
{
    static const char stop_msg[] = "[*] Caught signal, stopping...\n";

    if (g_running) {
        ssize_t nw = write(STDERR_FILENO, stop_msg, sizeof(stop_msg) - 1);
        (void)nw;
    }
    g_running = 0;
}

void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("sigaction(SIGTERM)");
        exit(EXIT_FAILURE);
    }
}

void status_msg(const char *fmt, ...)
{
    va_list ap;

    if (!fmt)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

int parse_u16_arg(const char *s, uint16_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);

    if (!s || !*s || (end && *end != '\0') || v > UINT16_MAX)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

int parse_u32_arg(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);

    if (!s || !*s || (end && *end != '\0') || v > UINT32_MAX)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

int parse_i32_arg(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);

    if (!s || !*s || (end && *end != '\0') || v < 0 || v > INT32_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

int split_argv_at_dd(int argc, char **argv,
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

void parse_app_args(int argc, char **argv, struct app_config *cfg)
{
    cfg->output_path           = UDP_DEFAULT_OUTPUT;
    cfg->dst_port              = 8888; /* TCP 业务默认与 dac UDP 一致 */
    cfg->expected_pkts         = 0;
    cfg->raw_payload           = 0;
    cfg->expected_payload_size = UDP_RX_PAYLOAD_SIZE;
    cfg->rx_cpu                = 0;
    cfg->writer_cpu            = 1;
    cfg->traffic_only          = 0;
    cfg->max_conns             = 1;

    if (argc <= 0)
        return;

    static const char opt_prog[] = "fstack_tcp_recv";
    char *optv[256];
    int optc = argc + 1;

    if (optc > (int)(sizeof(optv) / sizeof(optv[0])))
        goto usage_fail;

    optv[0] = (char *)opt_prog;
    for (int i = 0; i < argc; i++)
        optv[i + 1] = argv[i];

    static struct option long_opts[] = {
        {"output",         required_argument, NULL, 'o'},
        {"dst-port",       required_argument, NULL, 'p'},
        {"expected-pkts",  required_argument, NULL, 'e'},
        {"raw-payload",    no_argument,       NULL, 'r'},
        {"no-raw-payload", no_argument,       NULL, 'R'},
        {"payload-size",   required_argument, NULL, 's'},
        {"rx-cpu",         required_argument, NULL, 'x'},
        {"writer-cpu",     required_argument, NULL, 'w'},
        {"traffic-only",   no_argument,       NULL, 't'},
        {"max-conns",      required_argument, NULL, 'm'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(optc, optv, "o:p:e:rs:R:x:w:tm:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'o':
            cfg->output_path = optarg;
            break;
        case 'p':
            if (parse_u16_arg(optarg, &cfg->dst_port) < 0)
                goto usage_fail;
            break;
        case 'e':
            if (parse_u32_arg(optarg, &cfg->expected_pkts) < 0)
                goto usage_fail;
            break;
        case 'r':
            cfg->raw_payload = 1;
            break;
        case 'R':
            cfg->raw_payload = 0;
            break;
        case 's':
            if (parse_u32_arg(optarg, &cfg->expected_payload_size) < 0)
                goto usage_fail;
            break;
        case 'x':
            if (parse_i32_arg(optarg, &cfg->rx_cpu) < 0)
                goto usage_fail;
            break;
        case 'w':
            if (parse_i32_arg(optarg, &cfg->writer_cpu) < 0)
                goto usage_fail;
            break;
        case 't':
            cfg->traffic_only = 1;
            break;
        case 'm':
            if (parse_i32_arg(optarg, &cfg->max_conns) < 0 ||
                cfg->max_conns < 1 || cfg->max_conns > 64)
                goto usage_fail;
            break;
        default:
            goto usage_fail;
        }
    }
    return;

usage_fail:
    fprintf(stderr,
            "\033[31m[CLI参数错误]\033[0m %s: "
            "「--」之后的应用参数无效或格式不正确。\n",
            opt_prog);
    exit(EXIT_FAILURE);
}

int rx_should_idle_exit(uint32_t expected_pkts, uint64_t matched,
                        const struct udp_rx_burst_timing *burst)
{
    if (expected_pkts == 0 || UDP_RX_IDLE_EXIT_SEC == 0)
        return 0;
    if (!burst->active || matched == 0 || matched >= expected_pkts)
        return 0;

    uint64_t now_ns = udp_rx_monotonic_ns();

    return now_ns - burst->last_ns >=
           (uint64_t)UDP_RX_IDLE_EXIT_SEC * 1000000000ull;
}

void print_rx_stats(const struct rx_stats *s, uint32_t expected_pkts,
                    const struct udp_rx_burst_timing *burst,
                    double total_elapsed_sec,
                    const struct udp_writer_stats *ws)
{
    printf("\n");
    printf("========== F-Stack TCP RX Statistics ==========\n");
    printf("  Total RX reads     : %12" PRIu64 "\n", s->total_rx);
    printf("  Total RX bytes     : %12" PRIu64 "  (%.2f MB)\n",
           s->total_rx_bytes, (double)s->total_rx_bytes / (1024.0 * 1024.0));
    printf("  Frames matched     : %12" PRIu64 "\n", s->udp_matched);
    printf("  Frame payload bytes: %12" PRIu64 "  (%.2f MB)\n",
           s->udp_bytes, (double)s->udp_bytes / (1024.0 * 1024.0));
    printf("  Payload errors     : %12" PRIu64 "\n", s->payload_errors);
    printf("  Seq gaps           : %12" PRIu64 "\n", s->payload_validate.seq_gaps);
    printf("  Seq reorder        : %12" PRIu64 "\n", s->payload_validate.seq_reorder);
    printf("  Length mismatch    : %12" PRIu64 "\n",
           s->payload_validate.length_mismatch);
    printf("  Writer errors      : %12" PRIu64 "\n", s->writer_errors);
    printf("  Buffer drops       : %12" PRIu64 "\n", s->buf_drops);
    printf("  Oversize drops     : %12" PRIu64 "\n", s->oversize_drops);
    printf("  Ring full drops    : %12" PRIu64 "\n", s->ring_full_drops);
    printf("  Ring enqueue waits : %12" PRIu64 "\n", s->ring_enqueue_waits);
    printf("  Ring enqueue spins : %12" PRIu64 "\n", s->ring_enqueue_spins);
    if (s->ring_enqueue_waits > 0) {
        printf("  Ring spins / wait  : %12.1f\n",
               (double)s->ring_enqueue_spins / (double)s->ring_enqueue_waits);
    }
    printf("  Buffer pool peak   : %12" PRIu64 "\n", s->bufpool_peak);
    udp_rx_print_timing(expected_pkts, burst, s->total_rx, s->total_rx_bytes,
                        s->udp_matched, s->udp_bytes, total_elapsed_sec);
    if (ws)
        udp_writer_print_throughput(ws);
    printf("===============================================\n");
    fflush(stdout);
}

int rx_channel_init(void)
{
    if (g_rx_ring || g_desc_pool || g_cpbuf_pool) {
        fprintf(stderr, "rx_channel_init: channel already initialized\n");
        return -1;
    }

    unsigned ring_sz = rte_align32pow2(UDP_RX_DESC_COUNT + 1);

    g_desc_pool = rte_mempool_create("tcp_rx_desc_pool",
                                     UDP_RX_DESC_COUNT,
                                     sizeof(struct rxbuf_desc),
                                     UDP_RX_MEMPOOL_CACHE_SIZE,
                                     0, NULL, NULL, NULL, NULL,
                                     rte_socket_id(), 0);
    if (!g_desc_pool) {
        fprintf(stderr, "Failed to create desc pool: %s\n",
                rte_strerror(rte_errno));
        return -1;
    }

    g_cpbuf_pool = rte_mempool_create("tcp_rx_cpbuf_pool",
                                      UDP_RX_CPBUF_COUNT,
                                      UDP_RXBUF_SLOT_SIZE,
                                      UDP_RX_MEMPOOL_CACHE_SIZE, 0,
                                      NULL, NULL, NULL, NULL,
                                      rte_socket_id(), 0);
    if (!g_cpbuf_pool) {
        rx_channel_destroy();
        fprintf(stderr, "Failed to create cpbuf pool: %s\n",
                rte_strerror(rte_errno));
        return -1;
    }

    g_rx_ring = rte_ring_create("tcp_rx_ring", ring_sz, rte_socket_id(),
                                RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!g_rx_ring) {
        rx_channel_destroy();
        fprintf(stderr, "Failed to create ring: %s\n", rte_strerror(rte_errno));
        return -1;
    }

    status_msg("[*] TCP RX channel ready: desc=%u cpbuf=%u ring=%u",
               (unsigned)UDP_RX_DESC_COUNT, (unsigned)UDP_RX_CPBUF_COUNT,
               ring_sz);
    return 0;
}

void rx_channel_destroy(void)
{
    if (g_rx_ring) {
        rte_ring_free(g_rx_ring);
        g_rx_ring = NULL;
    }
    if (g_desc_pool) {
        rte_mempool_free(g_desc_pool);
        g_desc_pool = NULL;
    }
    if (g_cpbuf_pool) {
        rte_mempool_free(g_cpbuf_pool);
        g_cpbuf_pool = NULL;
    }
}

static int set_sock_bufs(int fd)
{
    int rcv = 16 * 1024 * 1024;
    int snd = 16 * 1024 * 1024;
    int on = 1;

    (void)ff_setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    (void)ff_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    (void)ff_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    return 0;
}

static int fstack_tcp_listen_setup(const struct app_config *cfg, int *listen_out)
{
    struct sockaddr_in6 addr;
    int on = 1;
    int fd;

    if (!cfg || !listen_out)
        return -1;

    fd = ff_socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[ERR] ff_socket(STREAM): %s\n", strerror(errno));
        return -1;
    }

    if (ff_ioctl(fd, FIONBIO, &on) < 0) {
        fprintf(stderr, "[ERR] FIONBIO listen: %s\n", strerror(errno));
        ff_close(fd);
        return -1;
    }

    set_sock_bufs(fd);

    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(cfg->dst_port);

    const char *bind_label = UDP_RX_BIND_IP6;
    if (inet_pton(AF_INET6, UDP_RX_BIND_IP6, &addr.sin6_addr) != 1) {
        fprintf(stderr, "[ERR] invalid UDP_RX_BIND_IP6\n");
        ff_close(fd);
        return -1;
    }

    if (ff_bind(fd, (const struct linux_sockaddr *)&addr, sizeof(addr)) < 0) {
        memset(&addr.sin6_addr, 0, sizeof(addr.sin6_addr));
        addr.sin6_addr = in6addr_any;
        bind_label = "::";
        if (ff_bind(fd, (const struct linux_sockaddr *)&addr, sizeof(addr)) < 0) {
            fprintf(stderr, "[ERR] ff_bind TCP failed: %s\n", strerror(errno));
            ff_close(fd);
            return -1;
        }
        status_msg("[!] ff_bind %s failed, fallback [::]:%u",
                   UDP_RX_BIND_IP6, (unsigned)cfg->dst_port);
    }

    if (ff_listen(fd, 128) < 0) {
        fprintf(stderr, "[ERR] ff_listen: %s\n", strerror(errno));
        ff_close(fd);
        return -1;
    }

    *listen_out = fd;
    g_listenfd = fd;
    status_msg("[*] TCP listen ready [%s]:%u (nonblocking)",
               bind_label, (unsigned)cfg->dst_port);
    return 0;
}

int fstack_udp_socket_setup(const struct app_config *cfg, int *sockfd_out)
{
    /* 兼容头文件声明：TCP 程序不走此路径 */
    (void)cfg;
    (void)sockfd_out;
    return -1;
}

void fstack_udp_socket_close(void)
{
    if (g_connfd >= 0) {
        ff_close(g_connfd);
        g_connfd = -1;
    }
    if (g_listenfd >= 0) {
        ff_close(g_listenfd);
        g_listenfd = -1;
    }
    g_sock_ready = 0;
}

int writer_thread_bind_cpu(int cpu)
{
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET((unsigned)cpu, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

void *writer_thread_main(void *arg)
{
    struct writer_ctx *ctx = (struct writer_ctx *)arg;

    if (writer_thread_bind_cpu(ctx->writer_cpu) != 0) {
        fprintf(stderr, "[ERR]:writer: pthread_setaffinity_np(cpu=%d) failed\n",
                ctx->writer_cpu);
    }

    struct udp_writer *writer = udp_writer_create(ctx->output_path, 0);
    if (!writer) {
        fprintf(stderr, "[ERR]:writer: udp_writer_create failed for %s\n",
                ctx->output_path);
        ctx->state = -1;
        return NULL;
    }

    ctx->state = 1;

    void *descs[UDP_RX_WRITER_DESC_BURST];
    void *cpbufs[UDP_RX_WRITER_DESC_BURST];
    void *desc_objs[UDP_RX_WRITER_DESC_BURST];

    for (;;) {
        unsigned n = rte_ring_sc_dequeue_burst(g_rx_ring, descs,
                                               UDP_RX_WRITER_DESC_BURST, NULL);
        if (n == 0) {
            if (ctx->rx_done && rte_ring_empty(g_rx_ring))
                break;
            rte_pause();
            continue;
        }

        unsigned n_cpbuf = 0, n_desc = 0;

        for (unsigned i = 0; i < n; i++) {
            struct rxbuf_desc *d = (struct rxbuf_desc *)descs[i];

            if (udp_writer_submit(writer, d->data, d->len) < 0)
                ctx->writer_errors++;

            if (d->cpbuf)
                cpbufs[n_cpbuf++] = d->cpbuf;
            desc_objs[n_desc++] = d;
        }

        if (n_cpbuf > 0)
            rte_mempool_put_bulk(g_cpbuf_pool, cpbufs, n_cpbuf);
        if (n_desc > 0)
            rte_mempool_put_bulk(g_desc_pool, desc_objs, n_desc);
    }

    if (udp_writer_flush(writer) < 0)
        ctx->writer_errors++;

    ctx->wstats = udp_writer_get_stats(writer);
    udp_writer_destroy(writer);
    return NULL;
}

int rx_extract_payload(const struct app_config *cfg,
                       const uint8_t *payload, uint32_t payload_len,
                       struct rx_stats *stats,
                       struct rx_loop_ctx *lctx,
                       const uint8_t **data_out, uint32_t *len_out)
{
    int ret;

    if (!cfg || !stats || !lctx || !data_out || !len_out)
        return -1;
    if (!payload || payload_len == 0) {
        stats->payload_errors++;
        return -1;
    }

    if (cfg->raw_payload) {
        ret = udp_payload_passthrough(payload, payload_len,
                                      cfg->expected_payload_size,
                                      &stats->payload_validate,
                                      data_out, len_out);
    } else {
        ret = udp_payload_extract(payload, payload_len,
                                  &lctx->next_expected_seq,
                                  &stats->payload_validate,
                                  data_out, len_out);
    }

    if (ret < 0) {
        stats->payload_errors++;
        return -1;
    }
    return 0;
}

int rx_enqueue_payload(const uint8_t *data, uint32_t len,
                       struct rx_stats *stats)
{
    struct rxbuf_desc *desc = NULL;
    uint8_t *cp = NULL;
    unsigned in_use;

    if (!data || !stats || len == 0)
        return -1;
    if (len > UDP_RXBUF_SLOT_SIZE) {
        stats->oversize_drops++;
        return -1;
    }

    if (rte_mempool_get(g_desc_pool, (void **)&desc) < 0) {
        stats->buf_drops++;
        return -1;
    }

    if (rte_mempool_get(g_cpbuf_pool, (void **)&cp) < 0) {
        rte_mempool_put(g_desc_pool, desc);
        stats->buf_drops++;
        return -1;
    }

    memcpy(cp, data, len);
    desc->data  = cp;
    desc->len   = len;
    desc->cpbuf = cp;

    {
        int waited = 0;

        for (;;) {
            if (rte_ring_sp_enqueue(g_rx_ring, desc) == 0)
                break;
            stats->ring_enqueue_spins++;
            waited = 1;
            if (!g_running) {
                stats->ring_enqueue_waits++;
                stats->ring_full_drops++;
                rte_mempool_put(g_cpbuf_pool, cp);
                rte_mempool_put(g_desc_pool, desc);
                return -1;
            }
            rte_pause();
        }
        if (waited)
            stats->ring_enqueue_waits++;
    }

    in_use = rte_mempool_in_use_count(g_desc_pool);
    if (in_use > stats->bufpool_peak)
        stats->bufpool_peak = in_use;

    return 0;
}

void rx_handle_datagram(struct rx_loop_ctx *lctx,
                        const uint8_t *buf, ssize_t n)
{
    /* TCP 路径用流式拼包，此函数仅满足头文件符号；不调用。 */
    (void)lctx;
    (void)buf;
    (void)n;
}

static void tcp_submit_frame(struct rx_loop_ctx *lctx,
                             const uint8_t *frame, uint32_t frame_len)
{
    const uint8_t *data = NULL;
    uint32_t len = 0;
    uint32_t submitted_seq;
    struct rx_stats *stats = lctx->stats;

    if (lctx->cfg->traffic_only) {
        /* 只按帧长计数，不做跨流序号校验、不入环落盘 */
        len = frame_len;
        stats->udp_matched++;
        stats->udp_bytes += len;
        submitted_seq = lctx->raw_seq++;
        udp_rx_burst_on_submit(lctx->burst, submitted_seq);
        return;
    }

    if (rx_extract_payload(lctx->cfg, frame, frame_len, stats, lctx,
                           &data, &len) < 0)
        return;

    if (len > UDP_RXBUF_SLOT_SIZE) {
        stats->oversize_drops++;
        return;
    }

    if (rx_enqueue_payload(data, len, stats) < 0)
        return;

    stats->udp_matched++;
    stats->udp_bytes += len;

    if (lctx->cfg->raw_payload)
        submitted_seq = lctx->raw_seq++;
    else
        submitted_seq = lctx->next_expected_seq - 1;

    udp_rx_burst_on_submit(lctx->burst, submitted_seq);
}

/* 从指定连接的 stream 缓冲拆帧 */
static int tcp_drain_frames_conn(struct rx_loop_ctx *lctx, struct tcp_conn *c)
{
    int nframes = 0;
    const struct app_config *cfg = lctx->cfg;

    for (;;) {
        uint32_t need;

        if (cfg->raw_payload || cfg->traffic_only) {
            need = cfg->expected_payload_size;
            if (need == 0 || c->stream_len < need)
                break;
            tcp_submit_frame(lctx, c->stream_buf, need);
            memmove(c->stream_buf, c->stream_buf + need, c->stream_len - need);
            c->stream_len -= need;
            nframes++;
        } else {
            uint32_t declared;

            if (c->stream_len < UDP_PAYLOAD_HDR_SIZE)
                break;
            memcpy(&declared, c->stream_buf + 4, 4);
            if (declared > UDP_RXBUF_SLOT_SIZE) {
                lctx->stats->payload_errors++;
                memmove(c->stream_buf, c->stream_buf + 1, c->stream_len - 1);
                c->stream_len -= 1;
                continue;
            }
            need = UDP_PAYLOAD_HDR_SIZE + declared;
            if (c->stream_len < need)
                break;
            tcp_submit_frame(lctx, c->stream_buf, need);
            memmove(c->stream_buf, c->stream_buf + need, c->stream_len - need);
            c->stream_len -= need;
            nframes++;
        }

        if (cfg->expected_pkts > 0 &&
            lctx->stats->udp_matched >= cfg->expected_pkts)
            break;
    }
    return nframes;
}

static void tcp_feed_bytes_conn(struct rx_loop_ctx *lctx, struct tcp_conn *c,
                                const uint8_t *buf, size_t n)
{
    struct rx_stats *stats = lctx->stats;

    if (n == 0)
        return;

    stats->total_rx++;
    stats->total_rx_bytes += (uint64_t)n;

    if (stats->total_rx == 1)
        status_msg("[*] First TCP data from F-Stack (%zu B)", n);

    if (!lctx->timing_active) {
        lctx->tsc_start_ns = udp_rx_monotonic_ns();
        lctx->timing_active = 1;
    }

    if (c->stream_len + n > TCP_STREAM_CAP) {
        status_msg("[!] stream buffer overflow (len=%zu + %zu); drop chunk",
                   c->stream_len, n);
        stats->payload_errors++;
        return;
    }

    memcpy(c->stream_buf + c->stream_len, buf, n);
    c->stream_len += n;
    (void)tcp_drain_frames_conn(lctx, c);
}

static void tcp_conn_close_at(int idx)
{
    if (idx < 0 || idx >= g_nconns)
        return;
    if (g_conns[idx].fd >= 0)
        ff_close(g_conns[idx].fd);
    free(g_conns[idx].stream_buf);
    g_conns[idx] = g_conns[g_nconns - 1];
    g_nconns--;
    g_connfd = (g_nconns > 0) ? g_conns[0].fd : -1;
    g_stream_len = (g_nconns > 0) ? g_conns[0].stream_len : 0;
}

static int tcp_try_accept(struct rx_loop_ctx *lctx)
{
    int maxc = lctx->cfg->max_conns;
    int accepted = 0;

    if (maxc > TCP_MAX_CONNS)
        maxc = TCP_MAX_CONNS;

    while (g_nconns < maxc) {
        int cfd = ff_accept(g_listenfd, NULL, NULL);
        int on = 1;
        uint8_t *sbuf;

        if (cfd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                status_msg("[!] ff_accept: %s", strerror(errno));
            break;
        }
        if (ff_ioctl(cfd, FIONBIO, &on) < 0) {
            status_msg("[!] conn FIONBIO failed: %s", strerror(errno));
            ff_close(cfd);
            continue;
        }
        set_sock_bufs(cfd);
        sbuf = malloc(TCP_STREAM_CAP);
        if (!sbuf) {
            status_msg("[!] malloc stream_buf failed");
            ff_close(cfd);
            break;
        }
        g_conns[g_nconns].fd = cfd;
        g_conns[g_nconns].stream_len = 0;
        g_conns[g_nconns].stream_buf = sbuf;
        g_nconns++;
        g_ever_connected = 1;
        g_connfd = cfd;
        accepted++;
        status_msg("[*] TCP accepted connfd=%d (active=%d/%d)",
                   cfd, g_nconns, maxc);
    }
    return accepted;
}

int rx_loop(void *arg)
{
    struct rx_loop_ctx *lctx = (struct rx_loop_ctx *)arg;
    static uint8_t rbuf[TCP_READ_CHUNK];
    static uint64_t last_hb_ns;
    int i, ci;

    if (!lctx || !lctx->cfg || !lctx->stats || !lctx->burst) {
        ff_stop_run();
        return 0;
    }

    if (!g_running)
        pipeline_exit_now(lctx, "Stopped by signal (Ctrl+C)");

    if (!g_sock_ready) {
        if (fstack_tcp_listen_setup(lctx->cfg, &g_listenfd) < 0) {
            fprintf(stderr, "[ERR] rx_loop: TCP listen setup failed\n");
            ff_stop_run();
            return 0;
        }
        g_sock_ready = 1;
        status_msg("[*] TCP listening; waiting for client connect "
                   "(max_conns=%d traffic_only=%d)",
                   lctx->cfg->max_conns, lctx->cfg->traffic_only);
        if (lctx->cfg->expected_pkts > 0 && UDP_RX_IDLE_EXIT_SEC > 0) {
            status_msg("[*] 连接并收满后或连续 %.0fs 无新帧退出",
                       (double)UDP_RX_IDLE_EXIT_SEC);
        }
    }

    {
        uint64_t now_ns = udp_rx_monotonic_ns();
        if (last_hb_ns == 0)
            last_hb_ns = now_ns;
        else if (now_ns - last_hb_ns >= 5000000000ull) {
            status_msg("[*] TCP RX progress: reads=%" PRIu64 " matched=%" PRIu64
                       " conns=%d",
                       lctx->stats->total_rx, lctx->stats->udp_matched,
                       g_nconns);
            last_hb_ns = now_ns;
        }
    }

    (void)tcp_try_accept(lctx);

    for (ci = 0; ci < g_nconns && g_running; ) {
        int closed = 0;

        for (i = 0; i < (int)UDP_RX_WRITER_DESC_BURST * 4 && g_running; i++) {
            ssize_t n = ff_read(g_conns[ci].fd, rbuf, sizeof(rbuf));
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    lctx->stats->payload_errors++;
                    status_msg("[!] ff_read conn=%d: %s",
                               g_conns[ci].fd, strerror(errno));
                }
                break;
            }
            if (n == 0) {
                status_msg("[*] peer closed TCP connection fd=%d",
                           g_conns[ci].fd);
                (void)tcp_drain_frames_conn(lctx, &g_conns[ci]);
                tcp_conn_close_at(ci);
                closed = 1;
                break;
            }
            tcp_feed_bytes_conn(lctx, &g_conns[ci], rbuf, (size_t)n);
            if (lctx->cfg->expected_pkts > 0 &&
                lctx->stats->udp_matched >= lctx->cfg->expected_pkts) {
                pipeline_exit_now(lctx, "COMPLETE");
            }
        }
        if (!closed)
            ci++;
    }

    if (lctx->cfg->expected_pkts > 0 &&
        lctx->stats->udp_matched >= lctx->cfg->expected_pkts) {
        pipeline_exit_now(lctx, "COMPLETE");
    }

    /*
     * 曾有连接且当前全部关闭后才做 idle 退出；
     * 多连接场景下仍有活跃连接时继续收。
     */
    if (g_ever_connected && g_nconns == 0 &&
        rx_should_idle_exit(lctx->cfg->expected_pkts,
                            lctx->stats->udp_matched, lctx->burst)) {
        pipeline_exit_now(lctx, "RX idle timeout");
    }

    return 0;
}

int run_recv_pipeline(const struct app_config *cfg)
{
    struct rx_stats stats;
    struct udp_rx_burst_timing burst;
    struct writer_ctx wctx;
    struct rx_loop_ctx lctx;
    pthread_t wtid;
    int writer_started = 0;
    uint64_t t0;
    double total_sec;
    struct udp_writer_stats empty_ws;

    if (!cfg) {
        fprintf(stderr, "[ERR] run_recv_pipeline: cfg=NULL\n");
        return -1;
    }

    memset(&stats, 0, sizeof(stats));
    memset(&burst, 0, sizeof(burst));
    memset(&wctx, 0, sizeof(wctx));
    memset(&lctx, 0, sizeof(lctx));
    memset(&empty_ws, 0, sizeof(empty_ws));

    wctx.output_path = cfg->output_path;
    wctx.writer_cpu  = cfg->writer_cpu;
    wctx.state       = 0;
    wctx.rx_done     = 0;

    lctx.cfg   = cfg;
    lctx.stats = &stats;
    lctx.burst = &burst;
    lctx.wctx  = &wctx;

    if (!cfg->traffic_only) {
        if (rx_channel_init() < 0) {
            fprintf(stderr, "[ERR] run_recv_pipeline: rx_channel_init failed\n");
            return -1;
        }

        if (pthread_create(&wtid, NULL, writer_thread_main, &wctx) != 0) {
            perror("pthread_create(writer)");
            rx_channel_destroy();
            return -1;
        }
        wctx.tid = wtid;
        wctx.tid_valid = 1;
        writer_started = 1;
        while (wctx.state == 0) {
            if (!g_running)
                break;
            rte_pause();
        }
        if (wctx.state < 0) {
            fprintf(stderr, "[ERR] run_recv_pipeline: writer init failed\n");
            pipeline_drain_writer(&wctx);
            rx_channel_destroy();
            return -1;
        }
    } else {
        status_msg("[*] traffic-only: skip writer / disk path");
        wctx.state = 1;
    }

    status_msg("[*] TCP RX pipeline ready, entering ff_run (Ctrl+C to stop)");

    lctx.pipeline_t0_ns = udp_rx_monotonic_ns();
    t0 = lctx.pipeline_t0_ns;
    ff_run(rx_loop, &lctx);
    total_sec = (double)(udp_rx_monotonic_ns() - t0) / 1e9;
    (void)total_sec;

    /* fallback：若未走 pipeline_exit_now，仍须 join writer 再 _exit */
    if (writer_started) {
        pipeline_drain_writer(&wctx);
        stats.writer_errors += wctx.writer_errors;
        pipeline_emit_stats(&lctx, &wctx.wstats);
        rx_channel_destroy();
    } else {
        pipeline_emit_stats(&lctx, &empty_ws);
    }
    _exit(stats.udp_matched >= cfg->expected_pkts ? 0 : 1);
}

int main(int argc, char *argv[])
{
    int ff_argc, app_argc;
    char **ff_argv, **app_argv;
    struct app_config cfg;

    install_signal_handlers();
    setlinebuf(stdout);

    if (split_argv_at_dd(argc, argv, &ff_argc, &ff_argv, &app_argc, &app_argv) != 0) {
        fprintf(stderr, "split_argv_at_dd failed\n");
        return EXIT_FAILURE;
    }

    if (app_argc == 0) {
        fprintf(stderr,
                "Hint: put application options after '--'\n"
                "  Example: %s -c configs/config.dac.ini -- "
                "--output test/output/tcp_live.bin --raw-payload "
                "--expected-pkts 100\n",
                argv[0]);
    }

    parse_app_args(app_argc, app_argv, &cfg);

    printf("[*] TCP Config: output=%s, dst_port=%u, expected_pkts=%u, "
           "rx_cpu=%d, writer_cpu=%d, traffic_only=%d, max_conns=%d\n",
           cfg.output_path, cfg.dst_port, cfg.expected_pkts,
           cfg.rx_cpu, cfg.writer_cpu, cfg.traffic_only, cfg.max_conns);
    if (cfg.traffic_only)
        status_msg("[*] Mode: traffic-only (multi-conn OK, no disk, no seq consistency)");
    else if (cfg.raw_payload)
        status_msg("[*] Frame mode: raw fixed-size chunks (expected=%u)",
                   cfg.expected_payload_size);
    else
        status_msg("[*] Frame mode: 8B proto header over TCP stream");

    if (ff_init(ff_argc, ff_argv) < 0) {
        fprintf(stderr, "ff_init failed\n");
        return EXIT_FAILURE;
    }

    install_signal_handlers();

    int ret = run_recv_pipeline(&cfg);
    return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
