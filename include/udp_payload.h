/*
 * include/udp_payload.h
 *
 * UDP payload 头部校验与数据区提取（DPDK / Linux 共用）。
 * payload 格式见 demo_udp_config.h。
 */
#ifndef UDP_PAYLOAD_H
#define UDP_PAYLOAD_H

#include "demo_udp_config.h"

#include <stdint.h>
#include <string.h>

struct udp_payload_validate_stats {
    uint64_t seq_gaps;
    uint64_t seq_reorder;
    uint64_t length_mismatch;
};

/*
 * 从 UDP payload 提取数据区。
 * next_expected_seq: 传入/传出期望序号；传 NULL 则跳过 seq 校验。
 * 返回 0 成功，<0 表示头部非法。
 */
static inline int udp_payload_extract(const uint8_t *payload, uint32_t payload_len,
                                      uint32_t *next_expected_seq,
                                      struct udp_payload_validate_stats *vstats,
                                      const uint8_t **data_out,
                                      uint32_t *data_len_out)
{
    if (!payload || !data_out || !data_len_out)
        return -1;

    if (payload_len < UDP_PAYLOAD_HDR_SIZE)
        return -1;

    uint32_t seq;
    uint32_t declared_len;
    memcpy(&seq, payload, 4);
    memcpy(&declared_len, payload + 4, 4);

    uint32_t available = payload_len - (uint32_t)UDP_PAYLOAD_HDR_SIZE;
    if (declared_len > available) {
        if (vstats)
            vstats->length_mismatch++;
        return -1;
    }

    if (next_expected_seq) {
        if (seq < *next_expected_seq) {
            if (vstats)
                vstats->seq_reorder++;
        } else if (seq > *next_expected_seq) {
            if (vstats)
                vstats->seq_gaps += (uint64_t)(seq - *next_expected_seq);
        }
        *next_expected_seq = seq + 1;
    }

    *data_out = payload + UDP_PAYLOAD_HDR_SIZE; // 剥离自定义协议的起始帧
    *data_len_out = declared_len;
    return 0;
}

/*
 * 透传模式：不剥离 8B 自定义协议头，整段 UDP payload 原样作为数据区落盘。
 * expected_size>0 时，统计实际 payload_len != expected_size 的包数
 *   （写入 vstats->length_mismatch），但不丢弃——按实际长度落盘。
 * 返回 0 成功，<0 表示非法。
 */
static inline int udp_payload_passthrough(const uint8_t *payload, uint32_t payload_len,
                                          uint32_t expected_size,
                                          struct udp_payload_validate_stats *vstats,
                                          const uint8_t **data_out,
                                          uint32_t *data_len_out)
{
    if (!payload || !data_out || !data_len_out)
        return -1;
    if (payload_len == 0)
        return -1;
    if (vstats && expected_size > 0 && payload_len != expected_size)
        vstats->length_mismatch++;
    *data_out = payload;             /* 不剥离，整段 payload 即数据区 */
    *data_len_out = payload_len;     /* 按实际长度落盘 */
    return 0;
}

#endif /* UDP_PAYLOAD_H */
