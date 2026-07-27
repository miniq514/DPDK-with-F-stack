#!/usr/bin/env bash
#
# sender/run_tcp_sender.sh — 内核 TCP 一键打流（不绑 DPDK）
#
# 用法:
#   sudo bash run_tcp_sender.sh --to-8004 --count 100
#   sudo bash run_tcp_sender.sh --to-8004 --count 100000 --rate-mbps 3000
#
# 时序：收端先 READY（fstack_tcp_recv listen），发端再 connect。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

CONFIG_H="${SCRIPT_DIR}/include/sender_config.h"
SEND_BIN="${SCRIPT_DIR}/../build/sender/tcp_stream_send"
SETUP_IPV6_SH="${SCRIPT_DIR}/../test/internet/setup_f1_ipv6.sh"

BIND_IF="${BIND_IF:-enp66s0f1}"
DST_MAC_8004="90:e2:ba:50:7c:f9"
DST_MAC_8007="90:e2:ba:50:aa:71"
DST_IP=""
DST_MAC=""
SRC_IP=""
COUNT="${COUNT:-100}"
RATE_MBPS="${RATE_MBPS:-0}"
PAYLOAD_SIZE="${PAYLOAD_SIZE:-1336}"
DST_PORT="${DST_PORT:-8888}"
RAW_PAYLOAD=1
TO_8004=1

usage() {
    cat <<EOF
Usage: sudo bash run_tcp_sender.sh [options]

  --to-8004 / --to-8007   目标（默认 --to-8004）
  --count N               帧数 (default ${COUNT})
  --rate-mbps M           目标 payload 比特率；0=尽力发
  --payload-size B        数据区字节 (default ${PAYLOAD_SIZE}；帧长=8+B)
  --dst-port P            目的端口 (default ${DST_PORT})
  --raw-payload           发送含 8B 头的完整帧（默认，对齐收端 --raw-payload）
  --no-raw-payload        同结构，收端用 8B 解析模式
  --bind-if NAME          本机万兆口 (default ${BIND_IF})
  -h, --help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --to-8004) TO_8004=1; shift ;;
        --to-8007) TO_8004=0; shift ;;
        --count) COUNT="$2"; shift 2 ;;
        --rate-mbps) RATE_MBPS="$2"; shift 2 ;;
        --payload-size) PAYLOAD_SIZE="$2"; shift 2 ;;
        --dst-port) DST_PORT="$2"; shift 2 ;;
        --raw-payload) RAW_PAYLOAD=1; shift ;;
        --no-raw-payload) RAW_PAYLOAD=0; shift ;;
        --bind-if) BIND_IF="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage; exit 1 ;;
    esac
done

if [[ "${TO_8004}" -eq 1 ]]; then
    DST_IP="1111:2222:1111:2222:1111:2222:1111:0003"
    DST_MAC="${DST_MAC_8004}"
    SRC_IP="1111:2222:1111:2222:1111:2222:1111:0002"
    LOCAL_ROLE="8007"
else
    DST_IP="1111:2222:1111:2222:1111:2222:1111:0002"
    DST_MAC="${DST_MAC_8007}"
    SRC_IP="1111:2222:1111:2222:1111:2222:1111:0003"
    LOCAL_ROLE="8004"
fi

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    echo "[FAIL] 请使用 sudo（配置 IPv6 / 邻居）" >&2
    exit 1
fi

# 编译
if [[ ! -x "${SEND_BIN}" ]]; then
    echo "[*] 编译 tcp_stream_send ..."
    ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
    cmake -S "${ROOT}" -B "${ROOT}/build" >/dev/null
    cmake --build "${ROOT}/build" --target tcp_stream_send -j"$(nproc)" >/dev/null
    [[ -x "${SEND_BIN}" ]] || { echo "[FAIL] 编译失败 ${SEND_BIN}" >&2; exit 1; }
fi

# 配本机 IPv6（内核态，不绑 DPDK）
if [[ -x "${SETUP_IPV6_SH}" ]]; then
    echo "[*] setup f1 IPv6 as ${LOCAL_ROLE}"
    bash "${SETUP_IPV6_SH}" "${LOCAL_ROLE}" || true
fi

sysctl -w "net.ipv6.conf.${BIND_IF}.accept_dad=0" >/dev/null 2>&1 || true
ip -6 addr replace "${SRC_IP}/64" dev "${BIND_IF}" 2>/dev/null || true
ip link set "${BIND_IF}" up 2>/dev/null || true

# 静态邻居：收端 DPDK/F-Stack 可能不回 NS
ip -6 neigh replace "${DST_IP}" lladdr "${DST_MAC}" dev "${BIND_IF}" \
    nud permanent 2>/dev/null || true

echo "[*] TCP sender → [${DST_IP}]:${DST_PORT} count=${COUNT} rate_mbps=${RATE_MBPS}"

ARGS=(--dst-ip "${DST_IP}" --dst-port "${DST_PORT}" --count "${COUNT}"
      --payload-size "${PAYLOAD_SIZE}")
if [[ "${RATE_MBPS}" != "0" ]]; then
    ARGS+=(--rate-mbps "${RATE_MBPS}")
fi
if [[ "${RAW_PAYLOAD}" -eq 1 ]]; then
    ARGS+=(--raw-payload)
else
    ARGS+=(--no-raw-payload)
fi

"${SEND_BIN}" "${ARGS[@]}"
