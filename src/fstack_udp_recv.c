/*
 * src/fstack_udp_recv.c
 *
 * F-Stack UDP 收包主程序（v1.0 骨架）。
 *
 * 架构：ff_run(RX 回调) + pthread(writer) + rte_ring → udp_writer_submit(io_uring)
 *
 * 运行示例（应用参数放在 "--" 之后）：
 *   sudo ./build/fstack_udp_recv -c configs/config.dac.ini -- \
 *       --output test/output/live.bin --dst-port 8888 --raw-payload
 *
 * 类型与函数说明见 include/fstack_udp_recv.h
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
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_errno.h>
#include <rte_common.h>

/* ============ 全局状态 ============ */

static volatile sig_atomic_t g_running = 1;

static struct rte_ring    *g_rx_ring    = NULL;
static struct rte_mempool *g_desc_pool  = NULL;
static struct rte_mempool *g_cpbuf_pool = NULL; //缓冲区、存在拷贝

static int g_sockfd = -1; /* UDP socket 文件描述符 */
static int g_sock_ready = 0; /* socket 须在 ff_run 回调内完成 bind */
static int g_stats_emitted = 0; /* 避免重复打印统计 */

/* Ctrl+C 后 ff_stop_run 退出会触发 F-Stack/DPDK double-free；先打统计再 _exit */
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

static void pipeline_exit_now(struct rx_loop_ctx *lctx, const char *reason)
{
    if (!lctx || !lctx->cfg || !lctx->stats)
        _exit(1);

    if (lctx->wctx)
        lctx->wctx->rx_done = 1;

    status_msg("[*] %s: matched %" PRIu64 "/%u packets (total_rx=%" PRIu64 ")",
               reason, lctx->stats->udp_matched, lctx->cfg->expected_pkts,
               lctx->stats->total_rx);

    pipeline_emit_stats(lctx, NULL);

    if (lctx->cfg->expected_pkts > 0 &&
        lctx->stats->udp_matched < lctx->cfg->expected_pkts) {
        fprintf(stderr,
                "[!] INCOMPLETE: matched %" PRIu64 "/%u packets — "
                "total_rx=%" PRIu64 "\n",
                lctx->stats->udp_matched, lctx->cfg->expected_pkts,
                lctx->stats->total_rx);
        fflush(stderr);
        _exit(1);
    }

    _exit(0);
}

/* ============ 信号与日志 ============ */

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
    /*
     * 不设置 SA_RESTART：确保 io_uring_wait_cqe 等阻塞点可被 Ctrl+C 打断，
     * RX 循环与 writer 线程通过 g_running / rx_done 协作退出。
     */
    if (sigaction(SIGINT, &sa, NULL) != 0) {  //Ctrl+C
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) { //kill
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

/* ============ 参数解析 ============ */

// 命令行参数，字符串转整数，out带出

int parse_u16_arg(const char *s, uint16_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10); //读取十进制无符号整数

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

/*
 * 在 argv 中查找 "--"，拆成 F-Stack 参数段与应用参数段。
 * 返回 0 成功；未找到 "--" 时 ff 段为完整 argv，app 段为空。
 */
int split_argv_at_dd(int argc, char **argv,
                            int *ff_argc, char ***ff_argv,
                            int *app_argc, char ***app_argv)
{
    int sep = -1;

    for (int i = 0; i < argc; i++) { 
        if (strcmp(argv[i], "--") == 0) { //找第一个分隔符
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
    cfg->output_path           = UDP_DEFAULT_OUTPUT; /* 宏见 demo_udp_config.h */
    cfg->dst_port              = UDP_DST_PORT;
    cfg->expected_pkts         = 0;  /* 预期收包数：0=持续运行至 Ctrl+C，CLI --expected-pkts 覆盖 */
    cfg->raw_payload           = 0;
    cfg->expected_payload_size = UDP_RX_PAYLOAD_SIZE; /* 1344，与 sender pcap 对齐 */
    cfg->rx_cpu                = 0;
    cfg->writer_cpu            = 1;

    if (argc <= 0)
        return;

    /*
     * split_argv_at_dd 传入的 argv 不含程序名（首项即 --output 等）。
     * getopt_long 要求 argv[0] 为占位程序名，否则会漏掉第一个长选项。
     */
    static const char opt_prog[] = "fstack_udp_recv";
    char *optv[256];
    int optc = argc + 1;

    if (optc > (int)(sizeof(optv) / sizeof(optv[0])))
        goto usage_fail;

    optv[0] = (char *)opt_prog;
    for (int i = 0; i < argc; i++)
        optv[i + 1] = argv[i];

    // getopt库提供的CLI解析
    static struct option long_opts[] = {
        {"output",         required_argument, NULL, 'o'},
        {"dst-port",       required_argument, NULL, 'p'},
        {"expected-pkts",  required_argument, NULL, 'e'},
        {"raw-payload",    no_argument,       NULL, 'r'},
        {"no-raw-payload", no_argument,       NULL, 'R'},
        {"payload-size",   required_argument, NULL, 's'},
        {"rx-cpu",         required_argument, NULL, 'x'},
        {"writer-cpu",     required_argument, NULL, 'w'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    optind = 1;
    // 分配解析方式
    while ((opt = getopt_long(optc, optv, "o:p:e:rs:R:x:w:", long_opts, NULL)) != -1) {
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
        default:
            goto usage_fail;
        }
    }
    return;

usage_fail: /* 未知选项或数值参数非法 */
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

/* ============ 统计输出（可按需扩展字段） ============ */

void print_rx_stats(const struct rx_stats *s, uint32_t expected_pkts,
                           const struct udp_rx_burst_timing *burst,
                           double total_elapsed_sec,
                           const struct udp_writer_stats *ws)
{
    printf("\n");
    printf("========== F-Stack UDP RX Statistics ==========\n");
    printf("  Total RX packets   : %12" PRIu64 "\n", s->total_rx);
    printf("  Total RX bytes     : %12" PRIu64 "  (%.2f MB)\n",
           s->total_rx_bytes, (double)s->total_rx_bytes / (1024.0 * 1024.0));
    printf("  UDP matched        : %12" PRIu64 "\n", s->udp_matched);
    printf("  UDP payload bytes  : %12" PRIu64 "  (%.2f MB)\n",
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

/* ============ RX 通道（ring + mempool） ============ */

/*
 * 创建 g_desc_pool、g_cpbuf_pool、g_rx_ring（须在 ff_init 之后调用）。
 */
int rx_channel_init(void)
{
    if (g_rx_ring || g_desc_pool || g_cpbuf_pool) {
        fprintf(stderr, "rx_channel_init: channel already initialized, here is a repeated attempt\n");
        return -1;
    }

    unsigned ring_sz = rte_align32pow2(UDP_RX_DESC_COUNT + 1);

    g_desc_pool = rte_mempool_create("udp_rx_desc_pool",
    UDP_RX_DESC_COUNT,
    sizeof(struct rxbuf_desc),
    UDP_RX_MEMPOOL_CACHE_SIZE,  //cache size,当前是256，可调
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    rte_socket_id(),
    0);

    if (!g_desc_pool) {
        fprintf(stderr, "Failed to create desc pool, count=%u: %s\n"
                "  hint: 增大 hugepages 或调小 UDP_RX_DESC_COUNT\n",
                (unsigned)UDP_RX_DESC_COUNT, rte_strerror(rte_errno));
        return -1;
    }

    g_cpbuf_pool = rte_mempool_create(
        "udp_rx_cpbuf_pool",
        UDP_RX_CPBUF_COUNT,
        UDP_RXBUF_SLOT_SIZE,
        UDP_RX_MEMPOOL_CACHE_SIZE, 0,
        NULL, NULL, NULL, NULL,
        rte_socket_id(), 0
    );

    if (!g_cpbuf_pool) {
        rx_channel_destroy();
        fprintf(stderr, "Failed to create cpbuf pool, count=%u slot=%uB: %s\n"
                "  hint: 增大 hugepages 或调小 UDP_RX_CPBUF_COUNT\n",
                (unsigned)UDP_RX_CPBUF_COUNT,
                (unsigned)UDP_RXBUF_SLOT_SIZE,
                rte_strerror(rte_errno));
        return -1;
    }

    g_rx_ring = rte_ring_create(
        "udp_rx_ring",
        ring_sz,
        rte_socket_id(),
        RING_F_SP_ENQ | RING_F_SC_DEQ   /* 单生产者入队、单消费者出队 */
    );

    if (!g_rx_ring) {
        rx_channel_destroy();
        fprintf(stderr, "Failed to create ring, size=%u: %s\n",
                ring_sz, rte_strerror(rte_errno));
        return -1;
    }

    status_msg("[*] RX channel ready: desc=%u cpbuf=%u (slot %u B) ring=%u",
               (unsigned)UDP_RX_DESC_COUNT, (unsigned)UDP_RX_CPBUF_COUNT,
               (unsigned)UDP_RXBUF_SLOT_SIZE, ring_sz);

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

/* ============ F-Stack UDP socket ============ */

/*
 * 创建非阻塞 IPv6 UDP socket 并 bind(::, cfg->dst_port)。
 * 须在 ff_init() 之后调用；成功写入 *sockfd_out 与全局 g_sockfd。
 * 监听任意本地 IPv6（::），发端仍发往 UDP_RX_BIND_IP6（…:0002）。
 */
int fstack_udp_socket_setup(const struct app_config *cfg, int *sockfd_out)
{
    struct sockaddr_in6 addr;
    int on = 1;
    int sockfd;

    if (!cfg || !sockfd_out)
        return -1;

    /* F-Stack 用户态协议栈上的 IPv6 UDP fd（非内核 socket） */
    sockfd = ff_socket(AF_INET6, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "[ERR] fstack_udp_socket_setup: ff_socket failed: %s\n",
                strerror(errno));
        return -1;
    }

    /*
     * 必须非阻塞：rx_loop 在 ff_run 回调里 ff_recvfrom，
     * 无包时应立刻 EAGAIN 返回，不能卡住整个 poll 循环。
     */
    if (ff_ioctl(sockfd, FIONBIO, &on) < 0) {
        fprintf(stderr, "[ERR] fstack_udp_socket_setup: FIONBIO failed: %s\n",
                strerror(errno));
        ff_close(sockfd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(cfg->dst_port);

    /*
     * 优先绑定收端地址 …:0002（与发端 dst / config addr6 一致）；
     * Link Down 等场景回退 ::（F-Stack 官方 IPv6 示例）。
     */
    const char *bind_label = UDP_RX_BIND_IP6;
    if (inet_pton(AF_INET6, UDP_RX_BIND_IP6, &addr.sin6_addr) != 1) {
        fprintf(stderr, "[ERR] fstack_udp_socket_setup: invalid UDP_RX_BIND_IP6\n");
        ff_close(sockfd);
        return -1;
    }

    if (ff_bind(sockfd, (const struct linux_sockaddr *)&addr,
                sizeof(addr)) < 0) {
        memset(&addr.sin6_addr, 0, sizeof(addr.sin6_addr));
        addr.sin6_addr = in6addr_any;
        bind_label = "::";
        if (ff_bind(sockfd, (const struct linux_sockaddr *)&addr,
                    sizeof(addr)) < 0) {
            fprintf(stderr,
                    "[ERR] fstack_udp_socket_setup: ff_bind [%s] and [::]:%u failed: %s\n",
                    UDP_RX_BIND_IP6, (unsigned)cfg->dst_port, strerror(errno));
            ff_close(sockfd);
            return -1;
        }
        status_msg("[!] ff_bind %s failed, fallback [::]:%u ok",
                   UDP_RX_BIND_IP6, (unsigned)cfg->dst_port);
    }

    *sockfd_out = sockfd;
    g_sockfd    = sockfd;

    status_msg("[*] UDP socket ready [%s]:%u (nonblocking)",
               bind_label, (unsigned)cfg->dst_port);

    return 0;
}

void fstack_udp_socket_close(void)
{
    if (g_sockfd >= 0) {
        ff_close(g_sockfd);
        g_sockfd = -1;
    }
    g_sock_ready = 0;
}

/* ============ writer 线程 ============ */

int writer_thread_bind_cpu(int cpu)
{
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET((unsigned)cpu, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

 /* 出环--写盘--还池--排空退出
    arg 为 writer_ctx 指针
 */
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
        // 出环,F-stack---Writer
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

        // 批量还池
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

/* ============ RX 数据处理 ============ */

/*
 * 按 cfg->raw_payload 选择透传或 8B 头解析。
 * 成功时 *data_out / *len_out 指向待落盘数据区，返回 0；失败返回 <0。
 */
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
        /* --raw-payload：整段 UDP payload 落盘，不剥 8B 头，会统计mismatch包数 */
        ret = udp_payload_passthrough(payload, payload_len,
                                      cfg->expected_payload_size,
                                      &stats->payload_validate,
                                      data_out, len_out);
    } else {
        /* 默认：剥离 seq(4) + len(4)，校验序号与声明长度 */
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

// 将data拷贝入g_rx_ring
int rx_enqueue_payload(const uint8_t *data, uint32_t len,
                              struct rx_stats *stats)
{
    struct rxbuf_desc *desc = NULL;
    uint8_t *cp = NULL; //拷贝承载区
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

    memcpy(cp, data, len); //Que:一次拷贝，可能存在优化空间
    desc->data  = cp;
    desc->len   = len;
    desc->cpbuf = cp;

    /* 自旋入队：环满时等 writer 消费；统计背压强度 */
    {
        int waited = 0;

        for (;;) {
            if (rte_ring_sp_enqueue(g_rx_ring, desc) == 0)
                break;
            stats->ring_enqueue_spins++;
            waited = 1;
            if (!g_running) { //放弃包
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

// 处理单个 ff_recvfrom 结果
void rx_handle_datagram(struct rx_loop_ctx *lctx,
                             const uint8_t *buf, ssize_t n)
{
    const uint8_t *data = NULL;
    uint32_t len = 0;
    uint32_t submitted_seq;
    struct rx_stats *stats;

    if (!lctx || !lctx->cfg || !lctx->stats || !lctx->burst || !buf || n <= 0)
        return;

    stats = lctx->stats;
    stats->total_rx++;
    stats->total_rx_bytes += (uint64_t)n;

    if (stats->total_rx == 1)
        status_msg("[*] First UDP datagram from F-Stack (%zd B payload)", n);

    if (!lctx->timing_active) {
        lctx->tsc_start_ns = udp_rx_monotonic_ns();
        lctx->timing_active = 1;
    }

    if (rx_extract_payload(lctx->cfg, buf, (uint32_t)n, stats, lctx,
                           &data, &len) < 0)
        return;

    if (len > UDP_RXBUF_SLOT_SIZE) {
        if (stats->oversize_drops == 0)
            status_msg("[!] payload data %u > cpbuf %u; "
                       "增大 UDP_RXBUF_SLOT_SIZE",
                       len, (unsigned)UDP_RXBUF_SLOT_SIZE);
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

/* ============ ff_run 回调（RX 主循环） ============ */

/*
 * F-Stack 每轮调用的 RX 循环。
 *   - ff_recvfrom(g_sockfd, ...) 非阻塞收包，每轮最多处理一批后返回交还主循环
 *   - 无包 (EAGAIN) 直接 return 0
 *   - 有包 → rx_handle_datagram
 *   - 检查 g_running / expected_pkts / idle exit → ff_stop_run() 优雅退出
 */
int rx_loop(void *arg)
{
    struct rx_loop_ctx *lctx = (struct rx_loop_ctx *)arg;
    /* 单 RX 线程，static 复用避免每轮栈分配；RX_MAX_UDP_PAYLOAD=65507 */
    static uint8_t buf[RX_MAX_UDP_PAYLOAD];
    static uint64_t last_hb_ns;
    static int recv_err_logged;
    /*
     * F-Stack 的 struct linux_sockaddr 仅 16 字节，IPv6 sockaddr_in6 为 28 字节。
     * freebsd2linux_sockaddr 会 bcopy 26 字节进 sa_data[14]，溢出栈 12 字节。
     * 用 sockaddr_storage（128B）承载，避免 stack smashing。
     */
    struct sockaddr_storage from;
    socklen_t fromlen;
    int i;

    if (!lctx || !lctx->cfg || !lctx->stats || !lctx->burst) {
        ff_stop_run();
        return 0;
    }

    if (!g_running) {
        pipeline_exit_now(lctx, "Stopped by signal (Ctrl+C)");
    }

    /* F-Stack 要求在 ff_run 回调内完成 socket bind */
    if (!g_sock_ready) {
        if (fstack_udp_socket_setup(lctx->cfg, &g_sockfd) < 0) {
            fprintf(stderr, "[ERR] rx_loop: socket setup failed\n");
            ff_stop_run();
            return 0;
        }
        g_sock_ready = 1;
        if (lctx->cfg->expected_pkts > 0 && UDP_RX_IDLE_EXIT_SEC > 0) {
            status_msg("[*] 等待发端首包（首包到达前不计 idle；收满后或连续 %.0fs 无新包退出）",
                       (double)UDP_RX_IDLE_EXIT_SEC);
        }
    }

    /* 每 5s 打印一次收包进度，便于区分「零包」与「未收满」 */
    {
        uint64_t now_ns = udp_rx_monotonic_ns();
        if (last_hb_ns == 0)
            last_hb_ns = now_ns;
        else if (now_ns - last_hb_ns >= 5000000000ull) {
            status_msg("[*] RX progress: total_rx=%" PRIu64 " matched=%" PRIu64
                       " payload_err=%" PRIu64 " (Ctrl+C 结束并打印统计)",
                       lctx->stats->total_rx, lctx->stats->udp_matched,
                       lctx->stats->payload_errors);
            last_hb_ns = now_ns;
        }
    }

    /* 单轮 burst 上限：与 writer 出环 burst 同量级，避免长占 ff_run */
    for (i = 0; i < (int)UDP_RX_WRITER_DESC_BURST * 4 && g_running; i++) {
        fromlen = sizeof(from);
        ssize_t n = ff_recvfrom(g_sockfd, buf, sizeof(buf), 0,
                                &from, &fromlen);
        if (n < 0) {
            /* 非阻塞 socket 无包：EAGAIN/EWOULDBLOCK 属正常，交还主循环 */
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                lctx->stats->payload_errors++;
                if (!recv_err_logged) {
                    status_msg("[!] ff_recvfrom error: %s", strerror(errno));
                    recv_err_logged = 1;
                }
            }
            break;
        }
        if (n == 0)
            break;
        rx_handle_datagram(lctx, buf, n);
    }

    /* 收满退出：直接 _exit，避免 F-Stack/DPDK teardown 的 double-free */
    if (lctx->cfg->expected_pkts > 0 &&
        lctx->stats->udp_matched >= lctx->cfg->expected_pkts) {
        pipeline_exit_now(lctx, "COMPLETE");
    }
    /* idle 超时退出（expected_pkts>0 且首包后 N 秒无新 matched 包） */
    if (rx_should_idle_exit(lctx->cfg->expected_pkts,
                            lctx->stats->udp_matched, lctx->burst)) {
        pipeline_exit_now(lctx, "RX idle timeout");
    }

    return 0;
}

/* ============ 运行编排 ============ */

/*
 * 组装 RX/writer 双线程全流程：
 *   rx_channel_init → pthread_create(writer) → 等 writer ready
 *   → fstack_udp_socket_setup → ff_run(rx_loop)
 *   → 收尾：置 rx_done、join writer、print_rx_stats
 */
int run_recv_pipeline(const struct app_config *cfg)
{
    struct rx_stats stats;
    struct udp_rx_burst_timing burst;
    struct writer_ctx wctx;
    struct rx_loop_ctx lctx;
    pthread_t wtid;
    uint64_t t0;
    double total_sec;

    if (!cfg) {
        fprintf(stderr, "[ERR] run_recv_pipeline: cfg=NULL\n");
        return -1;
    }

    memset(&stats, 0, sizeof(stats));
    memset(&burst, 0, sizeof(burst));
    memset(&wctx, 0, sizeof(wctx));
    memset(&lctx, 0, sizeof(lctx));

    wctx.output_path = cfg->output_path;
    wctx.writer_cpu  = cfg->writer_cpu;
    wctx.state       = 0;
    wctx.rx_done     = 0;

    lctx.cfg   = cfg;
    lctx.stats = &stats;
    lctx.burst = &burst;
    lctx.wctx  = &wctx;

    if (rx_channel_init() < 0) {
        fprintf(stderr, "[ERR] run_recv_pipeline: rx_channel_init failed\n");
        return -1;
    }

    /* 启动 writer 线程，等待其就绪（udp_writer_create 完成） */
    if (pthread_create(&wtid, NULL, writer_thread_main, &wctx) != 0) {
        perror("pthread_create(writer)");
        rx_channel_destroy();
        return -1;
    }
    while (wctx.state == 0) {
        if (!g_running) {
            /* 信号期间等待：让 writer 也看到退出意图 */
            break;
        }
        rte_pause();
    }
    if (wctx.state < 0) {
        fprintf(stderr, "[ERR] run_recv_pipeline: writer init failed\n");
        wctx.rx_done = 1;
        pthread_join(wtid, NULL);
        rx_channel_destroy();
        return -1;
    }

    /* socket 在 rx_loop（ff_run 回调）内延迟创建 */
    status_msg("[*] RX pipeline ready, entering ff_run (Ctrl+C to stop)");

    lctx.pipeline_t0_ns = udp_rx_monotonic_ns();
    t0 = lctx.pipeline_t0_ns;
    ff_run(rx_loop, &lctx);
    total_sec = (double)(udp_rx_monotonic_ns() - t0) / 1e9;

    /* 通知 writer 排空后退出 */
    wctx.rx_done = 1;
    pthread_join(wtid, NULL);
    stats.writer_errors += wctx.writer_errors;

    /*
     * 正常退出（收满/idle）已经在 rx_loop 内直接 _exit；
     * 走到这里只可能是 F-Stack teardown 出错前的 fallback，直接 _exit。
     */
    pipeline_emit_stats(&lctx, &wctx.wstats);
    _exit(stats.udp_matched >= cfg->expected_pkts ? 0 : 1);
}

/* ============ 入口 ============ */

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
                "--output test/output/live.bin --raw-payload\n",
                argv[0]);
    }

    parse_app_args(app_argc, app_argv, &cfg);

    printf("[*] Config: output=%s, dst_port=%u, expected_pkts=%u, "
           "rx_cpu=%d, writer_cpu=%d\n",
           cfg.output_path, cfg.dst_port, cfg.expected_pkts,
           cfg.rx_cpu, cfg.writer_cpu);
    if (cfg.raw_payload)
        status_msg("[*] Payload mode: raw passthrough (expected=%u)",
                   cfg.expected_payload_size);
    else
        status_msg("[*] Payload mode: 8B proto header");

    /* F-Stack + DPDK 初始化（消耗 -c config.ini 等参数） */
    if (ff_init(ff_argc, ff_argv) < 0) {
        fprintf(stderr, "ff_init failed\n");
        return EXIT_FAILURE;
    }

    /* ff_init 可能覆盖信号处理；收端统计依赖 SIGINT 置 g_running */
    install_signal_handlers();

    int ret = run_recv_pipeline(&cfg);

    return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
