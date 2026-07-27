#!/usr/bin/env bash
# scripts/setup_kni_ops.sh
#
# F-Stack KNI（virtio-user）起来后，给内核侧 veth0 配置运维 IPv6。
# 业务地址仍在 F-Stack addr6（…:0003）；运维用独立 …:0004，避免 ND 冲突。
#
# 用法（通常由 run_recv.sh --kni-ops 后台调用；也可手动）:
#   sudo ./scripts/setup_kni_ops.sh [--link dac|fibre] [--wait-sec 10]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/link_profile.sh
source "${SCRIPT_DIR}/lib/link_profile.sh"

LINK="dac"
WAIT_SEC=10

while [[ $# -gt 0 ]]; do
    case "$1" in
        --link)     LINK="$2"; shift 2 ;;
        --wait-sec) WAIT_SEC="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 1 ;;
    esac
done

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    echo "[FAIL] 请使用 sudo 运行" >&2
    exit 1
fi

link_profile_apply "${LINK}" || exit 1

IFACE="${LINK_OPS_IFACE}"
OPS_IP="${LINK_OPS_IPV6}"
PREFIX="${LINK_PREFIX}"

max_tries=$(( WAIT_SEC * 2 ))   # 0.5s 间隔
found=0
for ((i = 1; i <= max_tries; i++)); do
    if ip link show "${IFACE}" &>/dev/null; then
        found=1
        break
    fi
    sleep 0.5
done

if [[ "${found}" -ne 1 ]]; then
    echo "[FAIL] ${WAIT_SEC}s 内未出现 ${IFACE}（KNI/virtio-user 未就绪）" >&2
    ip -br link | head -40 >&2 || true
    exit 1
fi

echo "[*] KNI ops: 配置 ${IFACE} → ${OPS_IP}/${PREFIX}"
ip link set "${IFACE}" up
# 去掉可能残留的同前缀地址，再写入运维地址
ip -6 addr flush dev "${IFACE}" scope global 2>/dev/null || true
ip -6 addr add "${OPS_IP}/${PREFIX}" dev "${IFACE}" 2>/dev/null || \
    ip -6 addr replace "${OPS_IP}/${PREFIX}" dev "${IFACE}"

# 降低 DAD 等待（若内核仍做 DAD）
sysctl -w "net.ipv6.conf.${IFACE}.accept_dad=0" >/dev/null 2>&1 || true
sysctl -w "net.ipv6.conf.${IFACE}.dad_transmits=0" >/dev/null 2>&1 || true

echo "[OK] ${IFACE} 运维地址就绪："
ip -6 addr show dev "${IFACE}"
