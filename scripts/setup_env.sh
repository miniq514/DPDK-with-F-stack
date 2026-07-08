#!/usr/bin/env bash
# scripts/setup_env.sh
#
# 将指定万兆口从内核交给 DPDK（uio_pci_generic），供 F-Stack 收端使用。
# 仅操作 --link 对应的那张口，不动另一张。
#
# 用法:
#   sudo ./scripts/setup_env.sh [--link dac|fibre]

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
    echo "[FAIL] 请使用 sudo 运行（需要 vfio 绑定）" >&2
    exit 1
fi

link_profile_apply "${LINK}" || exit 1
link_profile_summary

echo "[*] 释放内核网口 ${LINK_IFACE}（删 IPv6、down）"
link_dpdk_iface_down "${LINK_IFACE}"

echo "[*] modprobe ${DPDK_BIND_DRIVER}"
modprobe "${DPDK_BIND_DRIVER}" 2>/dev/null || true

link_dpdk_bind_iommu_group_uio

echo "[OK] setup_env 完成（${LINK_NAME}: ${LINK_IFACE} 已交给 DPDK）"
echo "     同 IOMMU 组口 ${LINK_SIBLING_IFACE}（${LINK_SIBLING_PCI}）已一并绑 ${DPDK_BIND_DRIVER}。"
echo "     退出收包后请运行: sudo ./scripts/release_env.sh --link ${LINK_NAME}"
