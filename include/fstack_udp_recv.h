/*
 * include/fstack_udp_recv.h
 *
 * F-Stack UDP 收包主程序（fstack_udp_recv）的类型定义与函数说明。
 * 实现均在 src/fstack_udp_recv.c；本头文件供阅读、对照实现与后续单测引用。
 *
 * 数据流：
 *   默认: ff_recvfrom → rx_extract_payload → rx_enqueue_payload → rte_ring
 *   --udp-bypass: packet_dispatcher 解析 Eth/IP/UDP → 同上（不进 sockbuf）
 *        → writer_thread_main → udp_writer_submit(io_uring) → 落盘
 */
#ifndef FSTACK_UDP_RECV_H
#define FSTACK_UDP_RECV_H

#include "udp_payload.h"
#include "udp_rx_timing.h"
#include "udp_writer.h"

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ============ 常量 ============ */

/* UDP 单包 payload 上限（IPv4/IPv6 通用） */
#define RX_MAX_UDP_PAYLOAD  65507u

/* ============ RX → writer 环上描述符 ============ */

/*
 * 经 rte_ring 从 RX 线程交给 writer 线程的一包数据。
 * F-Stack 路径无 mbuf 零拷贝：payload 始终 memcpy 进 cpbuf 池槽位。
 */
struct rxbuf_desc {
    uint8_t  *data;    /* 待落盘数据起始（指向 cpbuf 内） */
    uint32_t  len;     /* 待落盘字节数 */
    uint8_t  *cpbuf;   /* g_cpbuf_pool 槽位；writer submit 后归还该池 */
};

/* ============ writer 线程上下文 ============ */

/*
 * pthread(writer) 入参；RX 与 writer 通过 state / rx_done 握手。
 */
struct writer_ctx {
    const char             *output_path;  /* 落盘文件路径 */
    int                     writer_cpu;   /* pthread_setaffinity_np 绑定核 */
    volatile int            state;        /* 0=初始化中, 1=就绪, -1=失败 */
    volatile int            rx_done;      /* RX 置 1 表示不再入环，writer 排空后退出 */
    uint64_t                writer_errors;/* udp_writer_submit/flush 失败计数 */
    struct udp_writer_stats wstats;       /* 退出前从 udp_writer 快照 */
    pthread_t               tid;          /* writer 线程；_exit 前须 join 以完成 ftruncate */
    int                     tid_valid;    /* 1=pthread_create 成功，可 join */
};

/* ============ 应用层配置（"--" 之后解析） ============ */

struct app_config {
    const char *output_path;           /* --output，默认 UDP_DEFAULT_OUTPUT */
    uint16_t    dst_port;              /* --dst-port，UDP bind 端口 */
    uint32_t    expected_pkts;         /* --expected-pkts，0=无限直到 Ctrl+C */
    int         raw_payload;           /* 1=整段 payload 透传；0=剥离 8B 头 */
    uint32_t    expected_payload_size; /* raw 模式期望长度（仅统计，不丢弃） */
    int         rx_cpu;                /* --rx-cpu，RX/ff_run 倾向 CPU */
    int         writer_cpu;            /* --writer-cpu，writer 线程绑定 CPU */
    int         traffic_only;          /* 1=只计流量，不落盘/不校验跨流序号 */
    int         max_conns;             /* TCP 最大并发连接（多核发端） */
    int         zc_recv;               /* 1=UDP 使用 ff_zc_recv 零拷贝收 */
    int         udp_bypass;            /* 1=半旁路：dispatcher 截获 dport，不进协议栈 socket */
};

/* ============ RX 统计 ============ */

struct rx_stats {
    uint64_t total_rx;       /* ff_recvfrom 成功次数 */
    uint64_t total_rx_bytes; /* 上述包字节合计 */
    uint64_t udp_matched;    /* payload 校验通过并成功入环的包数 */
    uint64_t udp_bytes;      /* 入环 payload 数据区字节合计 */
    uint64_t payload_errors; /* payload 解析失败 */
    uint64_t writer_errors;  /* 来自 writer 线程的写盘错误（汇总） */
    uint64_t buf_drops;      /* desc/cpbuf 池耗尽 */
    uint64_t oversize_drops; /* 数据区 > UDP_RXBUF_SLOT_SIZE */
    uint64_t ring_full_drops;/* rte_ring 满（实现中可忙等重试，此项为放弃时计数） */
    uint64_t ring_enqueue_waits; /* 入环时环满、至少自旋过一次的包数 */
    uint64_t ring_enqueue_spins; /* 入环自旋总次数（每次 enqueue 失败 + rte_pause 前计 1） */
    uint64_t bufpool_peak;   /* desc 池在途峰值 */
    struct udp_payload_validate_stats payload_validate; /* seq/长度校验 */
};

/* ============ ff_run 回调上下文 ============ */

struct rx_loop_ctx {
    const struct app_config    *cfg;
    struct rx_stats            *stats;
    struct udp_rx_burst_timing *burst;   /* 首包→末包 submit 窗口计时 */
    struct writer_ctx          *wctx;
    uint64_t                    pipeline_t0_ns;    /* ff_run 起始时刻（统计用） */
    uint32_t                    next_expected_seq; /* 8B 头模式序号 */
    uint32_t                    raw_seq;           /* raw 模式递增序号（仅统计） */
    uint64_t                    tsc_start_ns;      /* 首包时刻（总耗时） */
    int                         timing_active;
};

/* ============ 内部 API（实现在 src/fstack_udp_recv.c） ============ */

/* --- 信号与日志 --- */

/* SIGINT / SIGTERM 回调：置全局运行标志为 0，请求优雅退出 */
void signal_handler(int);

/* 注册 SIGINT、SIGTERM 处理函数 */
void install_signal_handlers(void);

/* 向 stderr 打印一行联调日志（自动换行并 fflush） */
void status_msg(const char *fmt, ...);

/* --- 参数解析 --- */

/* 将十进制字符串解析为 uint16_t；非法或溢出返回 -1 */
int parse_u16_arg(const char *s, uint16_t *out);

/* 将十进制字符串解析为 uint32_t；非法或溢出返回 -1 */
int parse_u32_arg(const char *s, uint32_t *out);

/* 将十进制字符串解析为非负 int；非法或溢出返回 -1 */
int parse_i32_arg(const char *s, int *out);

/*
 * 在 argv 中查找 "--"，拆成两段：
 *   "--" 之前 → ff_init（F-Stack / DPDK 参数）
 *   "--" 之后 → parse_app_args（应用参数）
 * 未找到 "--" 时整段 argv 视为 F-Stack 参数，应用段为空。返回 0。
 */
int split_argv_at_dd(int argc, char **argv,
                     int *ff_argc, char ***ff_argv,
                     int *app_argc, char ***app_argv);

/*
 * 解析 "--" 之后的应用参数（--output、--dst-port 等），写入 cfg。
 * 参数非法时打印 usage 并 exit(EXIT_FAILURE)。
 */
void parse_app_args(int argc, char **argv, struct app_config *cfg);

/*
 * expected_pkts>0 且已收到首包后：若连续 UDP_RX_IDLE_EXIT_SEC 秒
 * 无新的 matched 包，返回 1 表示应 idle 退出；否则返回 0。
 */
int rx_should_idle_exit(uint32_t expected_pkts, uint64_t matched,
                        const struct udp_rx_burst_timing *burst);

/* --- 统计 --- */

/* 进程结束前打印 RX 统计、burst 计时与 writer 吞吐 */
void print_rx_stats(const struct rx_stats *s, uint32_t expected_pkts,
                    const struct udp_rx_burst_timing *burst,
                    double total_elapsed_sec,
                    const struct udp_writer_stats *ws);

/* --- RX 通道（rte_ring + mempool）--- */

/*
 * 创建 g_desc_pool、g_cpbuf_pool、g_rx_ring（须在 ff_init 之后调用）。
 * 池/环参数见 demo_udp_config.h。成功返回 0，失败返回 -1。
 */
int rx_channel_init(void);

/* 释放 RX 环与两个 mempool（main 退出路径调用） */
void rx_channel_destroy(void);

/* --- F-Stack UDP socket --- */

/*
 * 创建非阻塞 UDP socket 并 bind 到 cfg->dst_port。
 * v1.0：AF_INET；v1.0.1：AF_INET6。成功写入 *sockfd_out，返回 0。
 */
int fstack_udp_socket_setup(const struct app_config *cfg, int *sockfd_out);

/* 关闭全局 g_sockfd */
void fstack_udp_socket_close(void);

/* --- writer 线程 --- */

/* 将当前线程绑定到指定 CPU；成功 0，失败 -1 */
int writer_thread_bind_cpu(int cpu);

/*
 * writer 线程入口：udp_writer_create → 标记就绪 → 从 ring 出队并 submit 落盘
 * → RX 结束且环空后 flush/destroy。返回 NULL。
 */
void *writer_thread_main(void *arg);

/* --- RX 数据处理 --- */

/*
 * 按 cfg->raw_payload 选择透传或 8B 头解析，得到待落盘数据指针与长度。
 * 成功返回 0；失败返回 <0 并更新 stats。
 */
int rx_extract_payload(const struct app_config *cfg,
                       const uint8_t *payload, uint32_t payload_len,
                       struct rx_stats *stats,
                       struct rx_loop_ctx *lctx,
                       const uint8_t **data_out, uint32_t *len_out);

/*
 * 从 cpbuf/desc 池取槽，memcpy  payload，经 sp_enqueue 送入 g_rx_ring。
 * 池或环失败时更新 stats 中对应 drop 计数。成功 0，失败 <0。
 */
int rx_enqueue_payload(const uint8_t *data, uint32_t len,
                       struct rx_stats *stats);

/*
 * 处理单次 ff_recvfrom 结果（n > 0）：更新 total_rx，
 * 调用 rx_extract_payload + rx_enqueue_payload，并更新 burst 计时。
 */
void rx_handle_datagram(struct rx_loop_ctx *lctx,
                        const uint8_t *buf, ssize_t n);

/* --- ff_run 与编排 --- */

/*
 * ff_run 每轮回调：非阻塞 recvfrom；EAGAIN 直接 return 0；
 * 有包则 rx_handle_datagram；收满/信号/idle 超时则 ff_stop_run()。
 */
int rx_loop(void *arg);

/*
 * 组装 RX/writer 双线程全流程：init 通道 → 启动 writer → socket setup
 * → ff_run(rx_loop) → join writer → print_rx_stats。成功 0，失败 <0。
 */
int run_recv_pipeline(const struct app_config *cfg);

#endif /* FSTACK_UDP_RECV_H */
