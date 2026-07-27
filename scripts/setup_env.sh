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

# shellcheck source=lib/hugepage.sh
source "${SCRIPT_DIR}/lib/hugepage.sh"
hugepages_ensure_inherit

# 确保 IOMMU 组为 identity（直通）模式。
# 8004 默认 IOMMU 为 Translated 模式，DPDK PA 模式 DMA 会被拦截（包到网卡但无法 DMA 到内存）。
# 8007 内核有 iommu=pt，8004 没有，需运行时切换。
ensure_iommu_identity() {
    local pci="${1}"
    local group_path group type
    group_path=$(readlink -f "/sys/bus/pci/devices/${pci}/iommu_group" 2>/dev/null) || return 0
    group=$(basename "${group_path}")
    type=$(cat "${group_path}/type" 2>/dev/null) || return 0

    if [[ "${type}" == "identity" ]]; then
        echo "[OK]  IOMMU group ${group} 已是 identity（直通）模式"
        return 0
    fi

    echo "[*] IOMMU group ${group} 当前为 ${type}，切换到 identity ..."
    # 解绑组内所有设备（否则切换会报 Device or resource busy）
    local dev driver
    for dev in $(ls "${group_path}/devices/" 2>/dev/null); do
        driver=$(readlink "/sys/bus/pci/devices/${dev}/driver" 2>/dev/null)
        driver=$(basename "${driver}" 2>/dev/null)
        if [[ -n "${driver}" ]]; then
            echo "    解绑 ${dev} (driver=${driver})"
            echo "${dev}" > "/sys/bus/pci/drivers/${driver}/unbind" 2>/dev/null || true
        fi
    done
    # 切换 IOMMU 组类型
    echo identity > "${group_path}/type" 2>/dev/null || {
        echo "[WARN] IOMMU group ${group} 切换 identity 失败，可能需要内核参数 iommu=pt" >&2
    }
    type=$(cat "${group_path}/type" 2>/dev/null)
    if [[ "${type}" == "identity" ]]; then
        echo "[OK]  IOMMU group ${group} 已切换为 identity"
    else
        echo "[WARN] IOMMU group ${group} 仍为 ${type}，DPDK 收包可能不工作" >&2
        echo "       永久修复: 在 /etc/default/grub 的 GRUB_CMDLINE_LINUX 添加 iommu=pt 然后 update-grub + 重启" >&2
    fi
    # 重新绑定非目标设备到原驱动（管理口等）
    for dev in $(ls "${group_path}/devices/" 2>/dev/null); do
        if [[ "${dev}" != "${pci}" && "${dev}" != "${LINK_SIBLING_PCI}" ]]; then
            local drv
            drv=$(lspci -k -s "${dev}" 2>/dev/null | grep 'Kernel driver:' | head -1 | awk '{print $3}')
            if [[ -n "${drv}" ]]; then
                echo "${dev}" > "/sys/bus/pci/drivers/${drv}/bind" 2>/dev/null || true
            fi
        fi
    done
}

ensure_iommu_identity "${LINK_PCI}"

echo "[*] 释放内核网口 ${LINK_IFACE}（删 IPv6、down）"
link_dpdk_iface_down "${LINK_IFACE}"

echo "[*] modprobe ${DPDK_BIND_DRIVER}"
modprobe "${DPDK_BIND_DRIVER}" 2>/dev/null || true

link_dpdk_bind_iommu_group_uio

echo "[OK] setup_env 完成（${LINK_NAME}: ${LINK_IFACE} 已交给 DPDK）"
echo "     同 IOMMU 组口 ${LINK_SIBLING_IFACE}（${LINK_SIBLING_PCI}）已一并绑 ${DPDK_BIND_DRIVER}。"
echo "     退出收包后请运行: sudo ./scripts/release_env.sh --link ${LINK_NAME}"
