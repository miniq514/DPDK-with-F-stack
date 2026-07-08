/*
 * sender/gen_udp_pcap.c
 *
 * 程序A：IPv6 UDP pcap 生成器（sender 工具包）。
 *
 * 生成与收端 include/udp_payload.h 解析一致的 IPv6 UDP pcap，供：
 *   - pktgen 重放打流（程序B run_pktgen_sender.sh）
 *   - 自环/离线回归测试
 *
 * 全程 IPv6：EtherType 0x86dd，IPv6 next_header=17(UDP)，UDP checksum 按
 * IPv6 pseudo header（src16+dst16+udp_len4+zero3+nexthdr1）计算。
 *
 * --to-8004 / --to-8007 切换打流目标（dst IP + dst MAC）；src 默认按目标反推。
 *
 * 用法:
 *   ./gen_udp_pcap <output.pcap> [options]
 *       --to-8007          打流目标 8007（默认）：dst …:0002 / MAC aa:71
 *       --to-8004          打流目标 8004：dst …:0003 / MAC 7c:f9
 *       --count N          包数量 (默认 100)
 *       --payload-size M   每包 payload 数据区字节数 (默认 1336；raw 下 UDP payload = 8B头+1336B体=1344B)
 *       --src-ip IP6       源 IPv6（默认按目标反推）
 *       --dst-ip IP6       目的 IPv6（默认按 --to-*）
 *       --src-mac MAC      源 MAC（默认按目标反推）
 *       --dst-mac MAC      目的 MAC（默认按 --to-*）
 *       --src-port PORT    源 UDP 端口 (默认 50001)
 *       --dst-port PORT    目的 UDP 端口 (默认 8888，与收端 bind 一致)
 *
 * payload 格式（与 udp_payload.h 一致）：
 *   [0..3]  uint32_t seq    (host order, 0-based)
 *   [4..7]  uint32_t length (host order, payload 数据区长度)
 *   [8..]   数据区：递增字节 ((i * payload_size + j) & 0xFF)，跨包连续
 */

#include "sender_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

/* ---- pcap 文件头 (24 字节) ---- */
struct pcap_file_hdr {
    uint32_t magic;
    uint16_t ver_major;
    uint16_t ver_minor;
    uint32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
};

/* ---- pcap 包记录头 (16 字节) ---- */
struct pcap_pkt_hdr {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};

struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;
};

/* IPv6 头部（40 字节），按 RFC 2460 布局 */
struct ip6_hdr {
    uint32_t ver_tc_fl;   /* version(6)+TC(8)+flow_label(20)，htons 后 0x60000000 */
    uint16_t payload_len; /* UDP header + payload（不含 IPv6 头自身） */
    uint8_t  nexthdr;     /* 17 = UDP */
    uint8_t  hlim;        /* hop limit */
    uint8_t  src[16];
    uint8_t  dst[16];
};

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;      /* UDP header + payload */
    uint16_t chksum;
};

/*
 * ones-complement checksum（与 IPv4/UDP 通用算法一致）。
 * buf 长度 n 为字节数；奇数尾部补 0 一字节。
 */
static uint16_t calc_chksum(const uint8_t *buf, int len)
{
    uint32_t sum = 0;
    int i;
    for (i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)((buf[i] << 8) | buf[i + 1]);
    if (i < len)            /* 奇数字节，左移补 0 */
        sum += (uint32_t)(buf[i] << 8);
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (uint16_t)(~sum);
}

/*
 * IPv6 UDP checksum：pseudo header = src(16) + dst(16) + udp_len(4 BE) +
 * zero(3) + nexthdr(1=17)，再接 UDP header + payload。
 * UDP 规定 0xFFFF 替代结果 0（0 在 UDP 表示无 checksum，IPv6 不允许）。
 */
static uint16_t udp6_chksum(const uint8_t src6[16], const uint8_t dst6[16],
                            const uint8_t *udp_seg, int udp_len)
{
    uint8_t ph[40];
    uint32_t ulen_be = htonl((uint32_t)udp_len);
    int total = (int)sizeof(ph) + udp_len;
    uint8_t *buf = malloc((size_t)total + 1);
    uint16_t cs;
    if (!buf)
        return 0;

    memcpy(ph,        src6, 16);
    memcpy(ph + 16,   dst6, 16);
    memcpy(ph + 32,   &ulen_be, 4);
    ph[36] = 0;
    ph[37] = 0;
    ph[38] = 0;
    ph[39] = 17;            /* nexthdr = UDP */

    memcpy(buf, ph, sizeof(ph));
    memcpy(buf + sizeof(ph), udp_seg, (size_t)udp_len);
    if (total % 2) {        /* 补零对齐 */
        buf[total] = 0;
        total++;
    }
    cs = calc_chksum(buf, total);
    free(buf);
    return cs ? cs : 0xFFFF;
}

/* 解析 "aa:bb:cc:dd:ee:ff" 到 6 字节；成功返回 0 */
static int parse_mac(const char *s, uint8_t out[6])
{
    unsigned int v[6];
    int n = sscanf(s, "%x:%x:%x:%x:%x:%x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    int i;
    if (n != 6)
        return -1;
    for (i = 0; i < 6; i++) {
        if (v[i] > 0xFF)
            return -1;
        out[i] = (uint8_t)v[i];
    }
    return 0;
}

static void fill_eth(struct eth_hdr *eth, const uint8_t dst_mac[6],
                     const uint8_t src_mac[6])
{
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, src_mac, 6);
    eth->ethertype = htons(0x86dd);   /* IPv6 */
}

static void fill_ip6(struct ip6_hdr *ip6, const uint8_t src6[16],
                     const uint8_t dst6[16], int udp_seg_len)
{
    ip6->ver_tc_fl    = htonl(0x60000000u);
    ip6->payload_len  = htons((uint16_t)udp_seg_len);  /* UDP hdr + payload */
    ip6->nexthdr      = 17;     /* UDP */
    ip6->hlim         = 64;
    memcpy(ip6->src, src6, 16);
    memcpy(ip6->dst, dst6, 16);
}

static int write_udp6_packet(FILE *f, int pkt_id,
                             const uint8_t src6[16], const uint8_t dst6[16],
                             uint16_t src_port, uint16_t dst_port,
                             const uint8_t dst_mac[6], const uint8_t src_mac[6],
                             uint32_t seq, const uint8_t *payload_data,
                             int payload_data_len)
{
    struct eth_hdr eth;
    struct ip6_hdr ip6;
    struct udp_hdr udp;
    uint8_t *udp_seg;
    uint8_t *frame;
    int full_payload_len, udp_seg_len, frame_len, off;
    struct pcap_pkt_hdr ph;
    uint32_t seq_h, len_h;

    fill_eth(&eth, dst_mac, src_mac);

    full_payload_len = (int)SEND_PAYLOAD_HDR_SIZE + payload_data_len;
    udp_seg_len      = 8 + full_payload_len;        /* UDP header(8) + payload */

    fill_ip6(&ip6, src6, dst6, udp_seg_len);

    udp.src_port = htons(src_port);
    udp.dst_port = htons(dst_port);
    udp.length   = htons((uint16_t)udp_seg_len);
    udp.chksum   = 0;

    udp_seg = malloc((size_t)udp_seg_len);
    if (!udp_seg)
        return -1;
    memcpy(udp_seg, &udp, 8);

    /* payload header: seq(4) + length(4)，host order（与 udp_payload.h 一致） */
    seq_h = seq;
    len_h = (uint32_t)payload_data_len;
    memcpy(udp_seg + 8, &seq_h, 4);
    memcpy(udp_seg + 12, &len_h, 4);
    if (payload_data_len > 0 && payload_data)
        memcpy(udp_seg + 8 + SEND_PAYLOAD_HDR_SIZE, payload_data,
               (size_t)payload_data_len);

    /* calc_chksum 按 big-endian 求和返回网络序整数值；存入 uint16_t 字段前需 htons
     * 转为 wire 字节序，否则在小端机上 memcpy 会把字节序反转，导致收端校验失败。 */
    udp.chksum = htons(udp6_chksum(src6, dst6, udp_seg, udp_seg_len));
    memcpy(udp_seg, &udp, 8);   /* 写回 checksum */

    frame_len = 14 + 40 + udp_seg_len;   /* eth + ipv6 + udp seg */
    frame = malloc((size_t)frame_len);
    if (!frame) {
        free(udp_seg);
        return -1;
    }
    off = 0;
    memcpy(frame + off, &eth, 14);  off += 14;
    memcpy(frame + off, &ip6, 40);  off += 40;
    memcpy(frame + off, udp_seg, (size_t)udp_seg_len);
    free(udp_seg);

    ph.ts_sec   = (uint32_t)pkt_id;   /* pcap 不用于定时，仅占位 */
    ph.ts_usec  = 0;
    ph.incl_len = (uint32_t)frame_len;
    ph.orig_len = (uint32_t)frame_len;
    if (fwrite(&ph, sizeof(ph), 1, f) != 1 ||
        fwrite(frame, (size_t)frame_len, 1, f) != 1) {
        free(frame);
        return -1;
    }
    free(frame);
    return frame_len;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <output.pcap> [options]\n"
        "  --to-8007          打流目标 8007 (默认): dst %s / mac %s\n"
        "  --to-8004          打流目标 8004: dst %s / mac %s\n"
        "  --count N          包数量 (默认 %u)\n"
        "  --payload-size M   每包 payload 数据区字节数 (默认 %u)\n"
        "  --src-ip IP6       源 IPv6 (默认按目标反推)\n"
        "  --dst-ip IP6       目的 IPv6 (默认按 --to-*)\n"
        "  --src-mac MAC      源 MAC (aa:bb:.. 形式)\n"
        "  --dst-mac MAC      目的 MAC (默认按 --to-*)\n"
        "  --src-port PORT    源端口 (默认 %u)\n"
        "  --dst-port PORT    目的端口 (默认 %u，与收端 bind 一致)\n"
        "\n"
        "说明:\n"
        "  - 生成的 pcap 与 fstack_udp_recv / udp_payload.h 解析兼容 (IPv6)\n"
        "  - EtherType 0x86dd，UDP checksum 按 IPv6 pseudo header 计算\n"
        "  - 速率由 pktgen 运行时 'set <port> rate <percent>' 控制，pcap 不写时间戳\n",
        prog,
        SEND_TARGET_8007_IP6, SEND_TARGET_8007_MAC,
        SEND_TARGET_8004_IP6, SEND_TARGET_8004_MAC,
        (unsigned)SEND_GEN_DEFAULT_COUNT,
        (unsigned)SEND_GEN_DEFAULT_PAYLOAD,
        (unsigned)SEND_UDP_SRC_PORT,
        (unsigned)SEND_UDP_DST_PORT);
}

int main(int argc, char *argv[])
{
    const char *filename;
    uint32_t num_pkts = SEND_GEN_DEFAULT_COUNT;
    uint32_t payload_size = SEND_GEN_DEFAULT_PAYLOAD;
    uint16_t dst_port = SEND_UDP_DST_PORT;
    uint16_t src_port = SEND_UDP_SRC_PORT;

    /* 目标：0=8007（默认），1=8004 */
    int target_8004 = 0;
    const char *dst_ip_str = NULL;
    const char *src_ip_str = NULL;
    const char *dst_mac_str = NULL;
    const char *src_mac_str = NULL;

    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint8_t src6[16];
    uint8_t dst6[16];
    uint8_t *payload_data;
    FILE *f;
    uint32_t pkt_id = 1;
    uint32_t i, j;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    filename = argv[1];

    {
        int ai;
        for (ai = 2; ai < argc; ai++) {
            if (!strcmp(argv[ai], "--to-8007")) {
                target_8004 = 0;
            } else if (!strcmp(argv[ai], "--to-8004")) {
                target_8004 = 1;
            } else if (!strcmp(argv[ai], "--count") && ai + 1 < argc) {
                char *end = NULL;
                unsigned long v = strtoul(argv[++ai], &end, 10);
                if (!end || *end != '\0' || v > 0xFFFFFFFFu) {
                    fprintf(stderr, "Invalid --count: %s\n", argv[ai]);
                    return 1;
                }
                num_pkts = (uint32_t)v;
            } else if (!strcmp(argv[ai], "--payload-size") && ai + 1 < argc) {
                char *end = NULL;
                unsigned long v = strtoul(argv[++ai], &end, 10);
                if (!end || *end != '\0' || v == 0 || v > 0xFFFFFFFFu) {
                    fprintf(stderr, "Invalid --payload-size: %s\n", argv[ai]);
                    return 1;
                }
                payload_size = (uint32_t)v;
            } else if (!strcmp(argv[ai], "--src-ip") && ai + 1 < argc) {
                src_ip_str = argv[++ai];
            } else if (!strcmp(argv[ai], "--dst-ip") && ai + 1 < argc) {
                dst_ip_str = argv[++ai];
            } else if (!strcmp(argv[ai], "--src-mac") && ai + 1 < argc) {
                src_mac_str = argv[++ai];
            } else if (!strcmp(argv[ai], "--dst-mac") && ai + 1 < argc) {
                dst_mac_str = argv[++ai];
            } else if (!strcmp(argv[ai], "--src-port") && ai + 1 < argc) {
                char *end = NULL;
                unsigned long v = strtoul(argv[++ai], &end, 10);
                if (!end || *end != '\0' || v > 65535) {
                    fprintf(stderr, "Invalid --src-port: %s\n", argv[ai]);
                    return 1;
                }
                src_port = (uint16_t)v;
            } else if (!strcmp(argv[ai], "--dst-port") && ai + 1 < argc) {
                char *end = NULL;
                unsigned long v = strtoul(argv[++ai], &end, 10);
                if (!end || *end != '\0' || v > 65535) {
                    fprintf(stderr, "Invalid --dst-port: %s\n", argv[ai]);
                    return 1;
                }
                dst_port = (uint16_t)v;
            } else if (!strcmp(argv[ai], "-h") || !strcmp(argv[ai], "--help")) {
                usage(argv[0]);
                return 0;
            } else {
                usage(argv[0]);
                return 1;
            }
        }
    }

    if (num_pkts == 0 || payload_size == 0) {
        fprintf(stderr, "Invalid packet count or payload size\n");
        return 1;
    }

    /* 解析 dst IP / MAC（按 --to-* 默认，可被 --dst-ip/--dst-mac 覆盖） */
    if (!dst_ip_str)
        dst_ip_str = target_8004 ? SEND_TARGET_8004_IP6 : SEND_TARGET_8007_IP6;
    if (!dst_mac_str)
        dst_mac_str = target_8004 ? SEND_TARGET_8004_MAC : SEND_TARGET_8007_MAC;
    if (parse_mac(dst_mac_str, dst_mac) < 0) {
        fprintf(stderr, "Invalid dst MAC (need aa:bb:..): %s\n", dst_mac_str);
        return 1;
    }

    /* src 默认按目标反推：发往 8007 → 本机=8004；发往 8004 → 本机=8007 */
    if (!src_ip_str)
        src_ip_str = target_8004 ? SEND_SRC_8007_IP6 : SEND_SRC_8004_IP6;
    if (!src_mac_str)
        src_mac_str = target_8004 ? SEND_SRC_8007_MAC : SEND_SRC_8004_MAC;
    if (parse_mac(src_mac_str, src_mac) < 0) {
        fprintf(stderr, "Invalid src MAC (need aa:bb:..): %s\n", src_mac_str);
        return 1;
    }

    if (inet_pton(AF_INET6, src_ip_str, src6) != 1 ||
        inet_pton(AF_INET6, dst_ip_str, dst6) != 1) {
        fprintf(stderr, "Invalid IPv6 address (src=%s dst=%s)\n",
                src_ip_str, dst_ip_str);
        return 1;
    }

    f = fopen(filename, "wb");
    if (!f) {
        perror("fopen");
        return 1;
    }

    /* pcap 文件头 */
    {
        struct pcap_file_hdr fh = {
            .magic = 0xa1b2c3d4, .ver_major = 2, .ver_minor = 4,
            .thiszone = 0, .sigfigs = 0, .snaplen = 65535, .linktype = 1,
        };
        if (fwrite(&fh, sizeof(fh), 1, f) != 1) {
            perror("fwrite pcap hdr");
            fclose(f);
            return 1;
        }
    }

    payload_data = malloc((size_t)payload_size);
    if (!payload_data) {
        perror("malloc");
        fclose(f);
        return 1;
    }

    for (i = 0; i < num_pkts; i++) {
        /* 跨包连续递增，收端落盘后满足 i & 0xFF 全局校验 */
        for (j = 0; j < payload_size; j++)
            payload_data[j] = (uint8_t)((i * payload_size + j) & 0xFF);

        if (write_udp6_packet(f, (int)pkt_id++, src6, dst6,
                              src_port, dst_port, dst_mac, src_mac,
                              i, payload_data, (int)payload_size) < 0) {
            fprintf(stderr, "Failed to write packet %u\n", i);
            free(payload_data);
            fclose(f);
            return 1;
        }
    }

    free(payload_data);
    fclose(f);

    {
        uint64_t total_payload = (uint64_t)num_pkts * (uint64_t)payload_size;
        printf("[+] Generated %s: %u IPv6 UDP packets, %u B payload each, "
               "%.2f MB total payload\n",
               filename, num_pkts, payload_size,
               (double)total_payload / (1024.0 * 1024.0));
        printf("    src=%s (%s) -> dst=%s:%u (%s)\n",
               src_ip_str, src_mac_str, dst_ip_str, (unsigned)dst_port,
               dst_mac_str);
        printf("    target=%s\n", target_8004 ? "8004" : "8007");
        printf("[*] Replay: pktgen -s 0:%s  (rate via 'set 0 rate <percent>')\n",
               filename);
    }
    return 0;
}
