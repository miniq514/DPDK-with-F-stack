/*
 * sender/include/sender_config.h
 *
 * sender 工具包 IPv6 配置常量（程序A gen_udp_pcap / 程序B run_pktgen_sender 共用）。
 * 全程 IPv6，与 docs/README.md 1.0.1 约束、test/internet 地址规划对齐：
 *   8007 收端  …:0002   8004 发端  …:0003   交换板  …:0001
 *
 * --to-8007 / --to-8004 切换打流目标；src 默认按"发往 8007 则本机=8004"反推。
 */
#ifndef SENDER_CONFIG_H
#define SENDER_CONFIG_H

/* ============ 目标（dst）地址 ============ */
#define SEND_TARGET_8007_IP6  "1111:2222:1111:2222:1111:2222:1111:0002"
#define SEND_TARGET_8004_IP6  "1111:2222:1111:2222:1111:2222:1111:0003"

/* dst MAC：收端万兆口绑 vfio 后不回 ARP，pcap 必须内嵌正确 MAC */
#define SEND_TARGET_8007_MAC  "90:e2:ba:50:aa:71"   /* 8007 f1（DAC 收端） */
#define SEND_TARGET_8004_MAC  "90:e2:ba:50:7c:f9"   /* 8004 f1（自环/反向收端） */

/* ============ 源（src）地址默认值 ============ */
/* 发往 8007 时本机默认 = 8004；发往 8004 时本机默认 = 8007 */
#define SEND_SRC_8004_IP6     "1111:2222:1111:2222:1111:2222:1111:0003"
#define SEND_SRC_8007_IP6     "1111:2222:1111:2222:1111:2222:1111:0002"
#define SEND_SRC_8004_MAC     "90:e2:ba:50:7c:f9"   /* 8004 f1 */
#define SEND_SRC_8007_MAC     "90:e2:ba:50:aa:71"   /* 8007 f1 */

/* ============ UDP 端口（与收端 ff_bind 一致） ============ */
#define SEND_UDP_DST_PORT     8888u
#define SEND_UDP_SRC_PORT     50001u

/* ============ 本机（发端）万兆口，pktgen 绑 vfio 用 ============ */
#define SEND_TX_PCI_DEFAULT   "0000:42:00.1"
#define SEND_TX_IF_DEFAULT    "enp66s0f1"
#define SEND_SSH_NIC_PCI      "0000:44:00.0"   /* eno4，禁止绑定 */

/* ============ Payload 格式（与 include/udp_payload.h 一致） ============ */
#define SEND_PAYLOAD_HDR_SIZE 8u    /* seq(4) + length(4) */
#define SEND_PAYLOAD_SIZE     1336u /* 数据区字节；raw 模式下 UDP payload = 8B头 + 1336B体 = 1344B */

/* ============ gen_udp_pcap 默认值 ============ */
#define SEND_GEN_DEFAULT_COUNT    100u
#define SEND_GEN_DEFAULT_PAYLOAD  SEND_PAYLOAD_SIZE

#endif /* SENDER_CONFIG_H */
