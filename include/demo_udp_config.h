/*
 * include/demo_udp_config.h
 *
 * UDP_DPDK 全局配置常量，供 DPDK / io_uring writer / pcap 生成器 / 脚本共用。
 */
#ifndef DEMO_UDP_CONFIG_H
#define DEMO_UDP_CONFIG_H

/* ============ 链路网段 ============ */
#define UDP_LOCAL_IP            "10.10.10.1"
#define UDP_REMOTE_IP           "10.10.10.2"
#define UDP_DST_PORT            50000
#define UDP_SRC_PORT            50001
#define UDP_NET_PREFIX          "24"

/* ============ MAC 地址（pcap 伪造用） ============ */
#define UDP_LOCAL_MAC_BYTES     0x00, 0x00, 0x00, 0x00, 0x00, 0x01
#define UDP_REMOTE_MAC_BYTES    0x00, 0x00, 0x00, 0x00, 0x00, 0x02

/* ============ 物理网卡 PCI ============ */
#define UDP_PCI_DEFAULT         "0000:42:00.1"
#define UDP_PCI_SSH_NIC         "0000:44:00.0"   /* eno4, 禁止绑定 */

/* ============ pcap / 输出路径 ============ */
#define UDP_DEFAULT_PCAP        "Test/udp_test.pcap"
#define UDP_DEFAULT_OUTPUT      "Test/udp_dpdk_output.bin"
#define UDP_LINUX_OUTPUT        "Test/udp_linux_output.bin"

/* ============ Payload 格式 ============ */
/*
 * payload 头部（8 字节）：
 *   uint32_t seq    — 包序号（0-based，host order）
 *   uint32_t length — payload 数据区长度（不含头部，host order）
 * 数据区：递增字节 (i & 0xFF)
 *
 * 魔数标识：payload 第一个 4 字节为 seq，方便 hash 校验。
 */
#define UDP_PAYLOAD_MAGIC       0x55445031u   /* "UDP1" */
#define UDP_PAYLOAD_HDR_SIZE    8u             /* seq(4) + length(4) */

/* ============ DPDK 参数 ============ */
#define UDP_NB_MBUFS            65535
#define UDP_MBUF_CACHE_SIZE     256
#define UDP_NB_RX_DESC          512   /* rte_eth_rx_queue_setup 的 nb_rx_desc */
#define UDP_NB_TX_DESC          512   /* rte_eth_tx_queue_setup 的 nb_tx_desc */
#define UDP_BURST_SIZE          32
#define UDP_MBUF_POOL_NAME      "udp_mbuf_pool"

/* ============ io_uring writer 参数 ============ */
#define UDP_WRITER_RING_ENTRIES     1024  /* io_uring SQ/CQ 深度 */
#define UDP_WRITER_MAX_PENDING_BUFS 128   /* 在途写 buffer 槽位数 */
#define UDP_WRITER_BUF_SIZE         (256 * 1024)  /* 单 buffer，须 >= AGG_SIZE */
#define UDP_WRITER_AGG_SIZE         (256 * 1024)  /* 写聚合目标大小 */
#define UDP_WRITER_SUBMIT_BATCH     32            /* SQE 累计到此数再 submit */

/* ============ O_DIRECT（v0.4.1 起默认启用，与 WriteBench bench_config.h 保持同步） ============ */
#define UDP_WRITER_USE_O_DIRECT     1   /* 1=绕过 page cache；0=普通 buffered 写 */
#define UDP_WRITER_IO_ALIGN         4096u /* buffer/offset/length 块对齐（512 或 4096） */

/* ============ RX/writer 解耦参数 ============ */
/*
 * F-Stack 路径：每包 1 desc + 1 cpbuf（无 mbuf 零拷贝），三者深度应对齐。
 *
 * 8 Gbps、UDP payload 1344B → ~744 kpps。
 * 8192 在途槽 ≈ 11 ms 线速缓冲，可吸收 writer/io_uring 抖动；
 * 16384 在途槽 ≈ 22 ms，更稳妥；cpbuf 内存 ≈ 16384×1344 ≈ 22 MB。
 * 程序 hugepage 预算 512×2MB=1GB 时，仍须为 F-Stack mbuf 池等预留大头。
 */
#define UDP_RX_PAYLOAD_SIZE         1344u  /* 业务固定 UDP payload（raw 透传） */
#define UDP_RXBUF_SLOT_SIZE         UDP_RX_PAYLOAD_SIZE
#define UDP_RX_DESC_COUNT           16384u /* 与 cpbuf 对齐：最大在途包数 */
#define UDP_RX_CPBUF_COUNT          16384u
#define UDP_RX_WRITER_DESC_BURST    32u    /* writer 单次出环 burst 大小 */
#define UDP_RX_MEMPOOL_CACHE_SIZE   256u   /* rte_mempool_create per-lcore cache */

/* ============ F-Stack 收端（IPv6，与 docs/README fibre/dac 1.0.1 对齐） ============ */
#define UDP_RX_BIND_IP6             "1111:2222:1111:2222:1111:2222:1111:0002"

#define UDP_GEN_DEFAULT_COUNT   100
#define UDP_GEN_DEFAULT_PAYLOAD 1400

/* ============ 收端行为 ============ */
/*
 * expected_pkts>0 时：收到首包后，若连续 N 秒无新 matched 包则自动退出。
 * 首包到达前不设超时，可无限等待发端启动。设为 0 则仅 Ctrl+C 或收满退出。
 */
#define UDP_RX_IDLE_EXIT_SEC    5u

/* ============ 安全边界 ============ */
#define UDP_NIC_CHECK_INTERVAL  5   /* 秒 */

#endif /* DEMO_UDP_CONFIG_H */
