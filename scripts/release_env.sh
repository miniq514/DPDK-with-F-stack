#!/usr/bin/env bash
# scripts/release_env.sh
#
# F-Stack / 收包进程退出后，把指定口还回内核并恢复联调 IPv6 地址。
# 仅操作 --link 对应的那张口，不动另一张。
#
# 用法:
#   sudo ./scripts/release_env.sh [--link dac|fibre]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/link_profile.sh
source "${SCRIPT_DIR}/lib/link_profile.sh"

LINK="dac"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --link) LINK="$2"; shift 2 ;;
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
link_profile_summary

link_dpdk_release_iommu_group_kernel

# 等待内核接口出现
for _ in 1 2 3 4 5; do
    if ip link show "${LINK_IFACE}" &>/dev/null; then
        break
    fi
    sleep 0.5
done

echo "[*] ip link set ${LINK_IFACE} up"
ip link set "${LINK_IFACE}" up 2>/dev/null || true

# fibre：显式设置 f0 MAC 与交换板 FPGA Dst_Mac 一致（90:e2:ba:50:aa:70）
if [[ "${LINK_NAME}" == "fibre" ]]; then
    echo "[*] ip link set address ${LINK_MAC} dev ${LINK_IFACE}（与交换板 FPGA 一致）"
    ip link set address "${LINK_MAC}" dev "${LINK_IFACE}" 2>/dev/null || \
        echo "[WARN] 设置 MAC 失败，可能已是目标 MAC 或硬件不支持" >&2
    ip link set "${LINK_IFACE}" up 2>/dev/null || true
fi

echo "[*] 恢复 IPv6: ${LINK_IPV6}/${LINK_PREFIX} dev ${LINK_IFACE}"
# 若已存在则不重复加
if ! ip -6 addr show dev "${LINK_IFACE}" | grep -q "${LINK_IPV6}"; then
    ip -6 addr add "${LINK_IPV6}/${LINK_PREFIX}" dev "${LINK_IFACE}"
else
    echo "[OK]  ${LINK_IPV6}/${LINK_PREFIX} 已存在，跳过"
fi

echo "[*] ip link set ${LINK_SIBLING_IFACE} up（同组口仅 up，不恢复 IP）"
ip link set "${LINK_SIBLING_IFACE}" up 2>/dev/null || true

echo "[OK] release_env 完成（${LINK_NAME}: ${LINK_IFACE} 已还回内核，IPv6 已恢复）"
echo "     同 IOMMU 组口 ${LINK_SIBLING_IFACE} 已还回 ${DPDK_KERNEL_DRIVER}。"
echo "     可 ping6 验证: ping6 -I ${LINK_IFACE} -c 3 ${LINK_REMOTE_IPV6}"
