/*
 * io-uring/udp_writer.c
 *
 * 共用 io_uring 写盘库实现。
 * 使用 io_uring 异步写文件，内部维护环形 buffer 池以避免数据拷贝竞争。
 *
 * 优化：写聚合（攒满 UDP_WRITER_AGG_SIZE 再 prep_write）+
 *       批量 submit（累计 UDP_WRITER_SUBMIT_BATCH 个 SQE 再 io_uring_submit）。
 * 可选：O_DIRECT + 块对齐 buffer（见 UDP_WRITER_USE_O_DIRECT）。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "udp_writer.h"
#include "../include/demo_udp_config.h"
#include "../include/udp_rx_timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <liburing.h>

/* ============ 内部常量 ============ */
#define DEFAULT_RING_ENTRIES  UDP_WRITER_RING_ENTRIES
#define WRITER_BUF_SIZE       UDP_WRITER_BUF_SIZE
#define MAX_PENDING_BUFS      UDP_WRITER_MAX_PENDING_BUFS
#define AGG_TARGET            UDP_WRITER_AGG_SIZE
#define SUBMIT_BATCH          UDP_WRITER_SUBMIT_BATCH
#define USE_O_DIRECT          UDP_WRITER_USE_O_DIRECT
#define IO_ALIGN              UDP_WRITER_IO_ALIGN

#if AGG_TARGET > WRITER_BUF_SIZE
#error "UDP_WRITER_AGG_SIZE must not exceed UDP_WRITER_BUF_SIZE"
#endif

#if USE_O_DIRECT
#if (AGG_TARGET % IO_ALIGN) != 0
#error "UDP_WRITER_AGG_SIZE must be a multiple of UDP_WRITER_IO_ALIGN for O_DIRECT"
#endif
#if (WRITER_BUF_SIZE % IO_ALIGN) != 0
#error "UDP_WRITER_BUF_SIZE must be a multiple of UDP_WRITER_IO_ALIGN for O_DIRECT"
#endif
#endif

/* ============ 内部辅助 ============ */

static int flush_sq(struct udp_writer *w);
static int submit_buf_write(struct udp_writer *w, int idx);
static int flush_staging(struct udp_writer *w);
static int maybe_flush_sq(struct udp_writer *w, int force);

static void record_cqe_complete(struct udp_writer *w)
{
    uint64_t now = udp_rx_monotonic_ns();

    if (!w->timing_active) {
        w->stats.first_cqe_ns = now;
        w->timing_active = 1;
    }
    w->stats.last_cqe_ns = now;
}

#if USE_O_DIRECT
static size_t align_up_size(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}
#endif

static void *alloc_writer_buf(void)
{
#if USE_O_DIRECT
    void *p = NULL;
    if (posix_memalign(&p, IO_ALIGN, WRITER_BUF_SIZE) != 0)
        return NULL;
    return p;
#else
    return malloc(WRITER_BUF_SIZE);
#endif
}

static void free_writer_buf(void *p)
{
    free(p);
}

static int reap_completions(struct udp_writer *w, int min_complete)
{
    struct io_uring_cqe *cqe;
    int reaped = 0;

    while (reaped < min_complete) {
        int ret = io_uring_wait_cqe(&w->ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "udp_writer: io_uring_wait_cqe: %s\n",
                    strerror(-ret));
            return -1;
        }

        int idx = (int)(uintptr_t)cqe->user_data;
        if (cqe->res < 0) {
            fprintf(stderr, "udp_writer: write error: %s\n",
                    strerror(-cqe->res));
            w->bufs[idx].in_flight = 0;
        } else {
            size_t written = (size_t)cqe->res;
            w->bufs[idx].done += written;

            if (w->bufs[idx].done < w->bufs[idx].len) {
                if (submit_buf_write(w, idx) < 0) {
                    fprintf(stderr, "udp_writer: short-write retry failed\n");
                    w->bufs[idx].in_flight = 0;
                    io_uring_cqe_seen(&w->ring, cqe);
                    return -1;
                }
            } else {
                w->stats.bytes_written += (uint64_t)w->bufs[idx].logical_len;
                record_cqe_complete(w);
                w->bufs[idx].in_flight = 0;
            }
        }

        io_uring_cqe_seen(&w->ring, cqe);
        reaped++;
    }
    return reaped;
}

static int maybe_flush_sq(struct udp_writer *w, int force)
{
    if (!force && w->sq_pending < SUBMIT_BATCH)
        return 0;
    if (w->sq_pending == 0)
        return 0;

    w->sq_pending = 0;
    w->stats.submit_count++;
    return flush_sq(w);
}

static int submit_buf_write(struct udp_writer *w, int idx)
{
    size_t remain = w->bufs[idx].len - w->bufs[idx].done;
    if (remain == 0)
        return 0;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&w->ring);
    if (!sqe) {
        if (maybe_flush_sq(w, 1) < 0)
            return -1;
        sqe = io_uring_get_sqe(&w->ring);
        if (!sqe) {
            if (reap_completions(w, 1) < 0)
                return -1;
            sqe = io_uring_get_sqe(&w->ring);
            if (!sqe) {
                fprintf(stderr, "udp_writer: SQ exhausted on retry\n");
                return -1;
            }
        }
    }

    io_uring_prep_write(sqe, w->fd,
                        w->bufs[idx].data + w->bufs[idx].done,
                        remain,
                        w->bufs[idx].file_offset + (off_t)w->bufs[idx].done);
    sqe->user_data = (uint64_t)(uintptr_t)idx;
    w->bufs[idx].in_flight = 1;
    w->stats.sqe_count++;
    w->sq_pending++;
    return maybe_flush_sq(w, 0);
}

static int flush_sq(struct udp_writer *w)
{
    for (;;) {
        int ret = io_uring_submit(&w->ring);
        if (ret >= 0)
            return 0;
        if (ret == -EBUSY) {
            if (reap_completions(w, 1) < 0)
                return -1;
            continue;
        }
        fprintf(stderr, "udp_writer: io_uring_submit: %s\n", strerror(-ret));
        return -1;
    }
}

static int acquire_buf_slot(struct udp_writer *w)
{
    for (int tries = 0; tries < (int)MAX_PENDING_BUFS; tries++) {
        int idx = w->next_buf;
        w->next_buf = (w->next_buf + 1) % MAX_PENDING_BUFS;
        if (idx == w->staging_idx)
            continue;
        if (!w->bufs[idx].in_flight)
            return idx;
    }

    w->stats.queue_full_count++;
    if (reap_completions(w, 1) < 0)
        return -1;

    for (int tries = 0; tries < (int)MAX_PENDING_BUFS; tries++) {
        int idx = w->next_buf;
        w->next_buf = (w->next_buf + 1) % MAX_PENDING_BUFS;
        if (idx == w->staging_idx)
            continue;
        if (!w->bufs[idx].in_flight)
            return idx;
    }

    fprintf(stderr, "udp_writer: no free buffer slot after reap\n");
    return -1;
}

static int ensure_staging(struct udp_writer *w)
{
    if (w->staging_idx >= 0)
        return 0;

    int idx = acquire_buf_slot(w);
    if (idx < 0)
        return -1;

    w->staging_idx = idx;
    w->staging_fill = 0;
    w->bufs[idx].len = 0;
    w->bufs[idx].logical_len = 0;
    w->bufs[idx].done = 0;
    w->bufs[idx].file_offset = w->write_offset;
    w->bufs[idx].in_flight = 0;
    return 0;
}

static int flush_staging(struct udp_writer *w)
{
    if (w->staging_idx < 0 || w->staging_fill == 0)
        return 0;

    int idx = w->staging_idx;
    w->bufs[idx].logical_len = w->staging_fill;
    w->bufs[idx].done = 0;

    // TIP：此处会把结尾的补成4KB倍数，对齐；后续再截断
#if USE_O_DIRECT
    size_t write_len = align_up_size(w->staging_fill, IO_ALIGN);
    if (write_len > w->staging_fill)
        memset(w->bufs[idx].data + w->staging_fill, 0,
               write_len - w->staging_fill);
    w->bufs[idx].len = write_len;
#else
    w->bufs[idx].len = w->staging_fill;
#endif

    if (submit_buf_write(w, idx) < 0)
        return -1;

    w->staging_idx = -1;
    w->staging_fill = 0;
    return 0;
}

/* ============ 公共接口 ============ */

struct udp_writer *udp_writer_create(const char *path, unsigned ring_entries)
{
    if (ring_entries == 0)
        ring_entries = DEFAULT_RING_ENTRIES;

    struct udp_writer *w = calloc(1, sizeof(*w));
    if (!w) {
        perror("udp_writer: calloc");
        return NULL;
    }

    w->ring_entries = ring_entries;
    w->staging_idx = -1;

    int open_flags = O_WRONLY | O_CREAT | O_TRUNC;
#if USE_O_DIRECT
    open_flags |= O_DIRECT;
#endif
    w->fd = open(path, open_flags, 0644);
    if (w->fd < 0) {
        perror("udp_writer: open");
        free(w);
        return NULL;
    }

    int ret = io_uring_queue_init(ring_entries, &w->ring, 0);
    if (ret < 0) {
        fprintf(stderr, "udp_writer: io_uring_queue_init: %s\n",
                strerror(-ret));
        close(w->fd);
        free(w);
        return NULL;
    }

    for (int i = 0; i < (int)MAX_PENDING_BUFS; i++) {
        w->bufs[i].data = alloc_writer_buf();
        if (!w->bufs[i].data) {
            perror("udp_writer: alloc buffer");
            for (int j = 0; j < i; j++)
                free_writer_buf(w->bufs[j].data);
            io_uring_queue_exit(&w->ring);
            close(w->fd);
            free(w);
            return NULL;
        }
        w->bufs[i].len = 0;
        w->bufs[i].logical_len = 0;
        w->bufs[i].done = 0;
        w->bufs[i].file_offset = 0;
        w->bufs[i].in_flight = 0;
    }
    w->buf_count = MAX_PENDING_BUFS;
    w->next_buf = 0;
    w->write_offset = 0;

#if USE_O_DIRECT
    printf("[+] udp_writer created: path=%s, ring=%u, bufs=%d, "
           "agg=%u KB, submit_batch=%u, O_DIRECT align=%u B\n",
           path, ring_entries, MAX_PENDING_BUFS,
           AGG_TARGET / 1024, SUBMIT_BATCH, (unsigned)IO_ALIGN);
#else
    printf("[+] udp_writer created: path=%s, ring=%u, bufs=%d, "
           "agg=%u KB, submit_batch=%u\n",
           path, ring_entries, MAX_PENDING_BUFS,
           AGG_TARGET / 1024, SUBMIT_BATCH);
#endif
    return w;
}

int udp_writer_submit(struct udp_writer *w, const void *buf, size_t len)
{
    if (!w || !buf || len == 0)
        return -EINVAL;

    const uint8_t *src = (const uint8_t *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        if (ensure_staging(w) < 0)
            return -1;

        size_t space = AGG_TARGET - w->staging_fill;
        size_t chunk = remaining < space ? remaining : space;

        memcpy(w->bufs[w->staging_idx].data + w->staging_fill, src, chunk);
        w->staging_fill += chunk;
        w->write_offset += (off_t)chunk;
        src += chunk;
        remaining -= chunk;

        if (w->staging_fill >= AGG_TARGET) {
            if (flush_staging(w) < 0)
                return -1;
        }
    }

    return 0;
}

int udp_writer_flush(struct udp_writer *w)
{
    if (!w)
        return -EINVAL;

    if (flush_staging(w) < 0)
        return -1;

    if (maybe_flush_sq(w, 1) < 0)
        return -1;

    int pending = 0;
    for (int i = 0; i < (int)MAX_PENDING_BUFS; i++) {
        if (w->bufs[i].in_flight)
            pending++;
    }

    if (pending > 0) {
        if (reap_completions(w, pending) < 0)
            return -1;
    }

#if USE_O_DIRECT
    if (ftruncate(w->fd, w->write_offset) < 0) {
        fprintf(stderr, "udp_writer: ftruncate: %s\n", strerror(errno));
        return -1;
    }
#endif

    fsync(w->fd);
    w->stats.flush_done_ns = udp_rx_monotonic_ns();
    return 0;
}

void udp_writer_destroy(struct udp_writer *w)
{
    if (!w)
        return;

    udp_writer_flush(w);

    printf("[+] udp_writer stats: syscall=%lu, sqe=%lu, written=%lu B (%.2f MB), "
           "queue_full=%lu\n",
           (unsigned long)w->stats.submit_count,
           (unsigned long)w->stats.sqe_count,
           (unsigned long)w->stats.bytes_written,
           (double)w->stats.bytes_written / (1024.0 * 1024.0),
           (unsigned long)w->stats.queue_full_count);

    for (int i = 0; i < (int)MAX_PENDING_BUFS; i++)
        free_writer_buf(w->bufs[i].data);

    io_uring_queue_exit(&w->ring);
    close(w->fd);
    free(w);
}

uint64_t udp_writer_bytes_written(const struct udp_writer *w)
{
    return w ? w->stats.bytes_written : 0;
}

struct udp_writer_stats udp_writer_get_stats(const struct udp_writer *w)
{
    if (w)
        return w->stats;
    struct udp_writer_stats zero = {0};
    return zero;
}

void udp_writer_print_throughput(const struct udp_writer_stats *s)
{
    if (!s || s->bytes_written == 0 || s->first_cqe_ns == 0)
        return;

    double disk_burst_sec;

    if (s->last_cqe_ns > s->first_cqe_ns)
        disk_burst_sec = (double)(s->last_cqe_ns - s->first_cqe_ns) / 1e9;
    else
        disk_burst_sec = 1e-9;

    double burst_mbps = s->bytes_written * 8.0 / disk_burst_sec / 1e6;
    double burst_mbs = (double)s->bytes_written / disk_burst_sec / (1024.0 * 1024.0);

    printf("  Elapsed time (disk burst) : %12.3f s  (first->last CQE)\n",
           disk_burst_sec);
    printf("  Throughput (disk, burst)  : %10.2f Mbps / %.2f MB/s\n",
           burst_mbps, burst_mbs);

    if (s->flush_done_ns > s->first_cqe_ns) {
        double disk_total_sec =
            (double)(s->flush_done_ns - s->first_cqe_ns) / 1e9;
        double total_mbps = s->bytes_written * 8.0 / disk_total_sec / 1e6;
        double total_mbs =
            (double)s->bytes_written / disk_total_sec / (1024.0 * 1024.0);

        if (disk_total_sec > disk_burst_sec + 0.001) {
            printf("  Elapsed time (disk total) : %12.3f s  (first CQE->fsync)\n",
                   disk_total_sec);
            printf("  Throughput (disk, total)  : %10.2f Mbps / %.2f MB/s\n",
                   total_mbps, total_mbs);
        }
    }
}
