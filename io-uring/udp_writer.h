/*
 * io-uring/udp_writer.h
 *
 * 共用 io_uring 写盘库接口。
 * DPDK 路径与 Linux 基线均使用此模块落盘，保证写盘逻辑一致。
 */
#ifndef UDP_WRITER_H
#define UDP_WRITER_H

#include "../include/demo_udp_config.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <liburing.h>

/* 统计信息 */
struct udp_writer_stats {
    uint64_t submit_count;       /* io_uring_submit 系统调用次数 */
    uint64_t sqe_count;          /* prep_write SQE 数量 */
    uint64_t bytes_written;      /* CQE 确认的逻辑写入字节数 */
    uint64_t queue_full_count;   /* 队列满、需等待的次数 */
    uint64_t first_cqe_ns;       /* 首个 buffer 完整落盘 CQE 时刻 */
    uint64_t last_cqe_ns;        /* 末个 buffer 完整落盘 CQE 时刻（flush 前） */
    uint64_t flush_done_ns;      /* fsync 完成时刻 */
};

/* ============ 实例内部结构（布局公开，便于阅读与调试） ============ */

/* 单个在途写 buffer：对应一次（或聚合后一次）prep_write */
struct pending_buf {
    char    *data;
    size_t   len;          /* 物理写长度（O_DIRECT 下可能含尾部 zero-pad） */
    size_t   logical_len;  /* 实际用户数据长度 */
    size_t   done;
    off_t    file_offset;
    int      in_flight;
};

struct udp_writer {
    int                  fd;
    struct io_uring      ring;
    unsigned             ring_entries;

    struct pending_buf   bufs[UDP_WRITER_MAX_PENDING_BUFS];
    int                  buf_count;
    int                  next_buf;

    off_t                write_offset;

    /* 写聚合：在单个 pending buffer 内顺序攒批 */
    int                  staging_idx;
    size_t               staging_fill;

    /* 批量 submit */
    unsigned             sq_pending;

    struct udp_writer_stats stats;
    int                  timing_active;
};

/* ============ 公共 API ============ */

/*
 * 创建 udp_writer 实例。
 *   path        — 输出文件路径
 *   ring_entries — io_uring 队列深度（0 则用 demo_udp_config.h 默认值）
 * 返回 NULL 表示失败。
 */
struct udp_writer *udp_writer_create(const char *path, unsigned ring_entries);

/*
 * 提交一段数据到 io_uring 写盘队列。
 * 数据会被拷贝到内部 buffer，调用者可立即释放 buf。
 * 返回 0 成功，<0 失败。
 */
int udp_writer_submit(struct udp_writer *w, const void *buf, size_t len);

/*
 * 刷新所有未完成的写操作，阻塞直到全部落盘。
 * 返回 0 成功，<0 失败。
 */
int udp_writer_flush(struct udp_writer *w);

/*
 * 销毁 udp_writer，关闭文件，释放资源。
 */
void udp_writer_destroy(struct udp_writer *w);

/*
 * 获取已写入总字节数。
 */
uint64_t udp_writer_bytes_written(const struct udp_writer *w);

/*
 * 获取当前统计快照。
 */
struct udp_writer_stats udp_writer_get_stats(const struct udp_writer *w);

/*
 * 打印落盘吞吐（CQE 确认口径），供 live 日志解析。
 * bytes_written / (first_cqe -> last_cqe) 为 burst；
 * bytes_written / (first_cqe -> fsync) 为 total。
 */
void udp_writer_print_throughput(const struct udp_writer_stats *s);

#endif /* UDP_WRITER_H */
