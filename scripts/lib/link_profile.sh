# scripts/lib/link_profile.sh
#
# 被 setup_env.sh / release_env.sh / run_recv.sh source；统一 --link dac|fibre
# 与路径/PCI/IPv6/端口变量，避免各脚本重复硬编码。
#
# 用法（一般不直接执行）：
#   source "$(dirname "$0")/lib/link_profile.sh"
#   link_profile_apply dac      # 或 fibre
#   echo "$LINK_IFACE $LINK_PCI $LINK_IPV6 $LINK_DST_PORT $FSTACK_CONFIG"
#
# 1.0.1 起全 IPv6：dac 与 fibre 收端地址均为 …:0002；同一时刻只启用一条链路。
#
# f0/f1（0000:42:00.0 / .1）同属一块 82599 的 IOMMU group；DPDK/VFIO 要求组内
# 设备全部绑 vfio-pci（或全部未绑），仅绑单口会报 "VFIO group is not viable"。

# 默认根（自动推断：本文件位于 <root>/scripts/lib/）
SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_ROOT="$(cd "${SCRIPTS_DIR}/.." && pwd)"

FF_PATH="${FF_PATH:-/home/dev/HMH--8007/F-stack}"
DEVBIND="${DEVBIND:-/usr/local/bin/dpdk-devbind.py}"
# 本机 82599 与 PCIe 桥/igc 管理口同属 IOMMU group 11，vfio-pci 无法使组 viable；
# uio_pci_generic 可单口绑定且已被 F-Stack/DPDK 支持。
DPDK_BIND_DRIVER="${DPDK_BIND_DRIVER:-uio_pci_generic}"
DPDK_KERNEL_DRIVER="${DPDK_KERNEL_DRIVER:-ixgbe}"

# 收端 IPv6（dac / fibre 收端统一 …:0002，与 docs/README 1.0.1 对齐）
LINK_IPV6_RECV="1111:2222:1111:2222:1111:2222:1111:0002"
LINK_IPV6_PREFIX=64
LINK_UDP_PORT=8888

link_profile_apply() {
    local link="${1:-dac}"
    case "${link}" in
        dac)
            LINK_IFACE="enp66s0f1"
            LINK_PCI="0000:42:00.1"
            LINK_SIBLING_IFACE="enp66s0f0"
            LINK_SIBLING_PCI="0000:42:00.0"
            LINK_MAC="90:e2:ba:50:aa:71"            # 8007 f1（dac 收端）
            LINK_REMOTE_IPV6="1111:2222:1111:2222:1111:2222:1111:0003"  # 8004 发端
            FSTACK_CONFIG="${PROJECT_ROOT}/configs/config.dac.ini"
            ;;
        fibre)
            LINK_IFACE="enp66s0f0"
            LINK_PCI="0000:42:00.0"
            LINK_SIBLING_IFACE="enp66s0f1"
            LINK_SIBLING_PCI="0000:42:00.1"
            LINK_MAC="90:e2:ba:50:aa:70"            # 8007 f0（与交换板 FPGA Dst_Mac 一致）
            LINK_REMOTE_IPV6="1111:2222:1111:2222:1111:2222:1111:0001"  # 交换板发端
            FSTACK_CONFIG="${PROJECT_ROOT}/configs/config.fibre.ini"
            ;;
        *)
            echo "[FAIL] link_profile_apply: 未知 --link '${link}'（应为 dac|fibre）" >&2
            return 1
            ;;
    esac

    LINK_NAME="${link}"
    LINK_IPV6="${LINK_IPV6_RECV}"
    LINK_PREFIX="${LINK_IPV6_PREFIX}"
    LINK_DST_PORT="${LINK_UDP_PORT}"
    export LINK_IFACE LINK_PCI LINK_SIBLING_IFACE LINK_SIBLING_PCI \
           LINK_MAC LINK_NAME LINK_IPV6 LINK_PREFIX \
           LINK_DST_PORT LINK_REMOTE_IPV6 FSTACK_CONFIG FF_PATH DEVBIND PROJECT_ROOT \
           DPDK_BIND_DRIVER DPDK_KERNEL_DRIVER
}

link_dpdk_pci_driver() {
    local pci="$1"
    if [[ ! -x "${DEVBIND}" ]]; then
        return 1
    fi
    "${DEVBIND}" -s 2>/dev/null | awk -v pci="${pci}" '
        $0 ~ pci {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^drv=/) {
                    sub(/^drv=/, "", $i)
                    print $i
                    exit
                }
            }
        }
    '
}

link_dpdk_iface_down() {
    local iface="$1"
    if ip link show "${iface}" &>/dev/null; then
        ip -6 addr flush dev "${iface}" 2>/dev/null || true
        ip addr flush dev "${iface}" 2>/dev/null || true
        ip link set "${iface}" down 2>/dev/null || true
    fi
}

link_dpdk_bind_uio() {
    local pci="$1"
    local iface="${2:-}"
    local driver

    if [[ ! -x "${DEVBIND}" ]]; then
        echo "[FAIL] 未找到 ${DEVBIND}" >&2
        return 1
    fi

    driver="$(link_dpdk_pci_driver "${pci}" || true)"
    if [[ "${driver}" == "${DPDK_BIND_DRIVER}" ]]; then
        echo "[OK]  ${pci} 已是 ${DPDK_BIND_DRIVER}"
        return 0
    fi

    [[ -n "${iface}" ]] && link_dpdk_iface_down "${iface}"
    echo "[*] dpdk-devbind: ${pci} → ${DPDK_BIND_DRIVER}"
    "${DEVBIND}" --force -b "${DPDK_BIND_DRIVER}" "${pci}"
    driver="$(link_dpdk_pci_driver "${pci}" || true)"
    if [[ "${driver}" == "${DPDK_BIND_DRIVER}" ]]; then
        echo "[OK]  ${pci} 已绑 ${DPDK_BIND_DRIVER}"
        return 0
    fi

    echo "[FAIL] ${DPDK_BIND_DRIVER} 绑定后仍未生效: ${pci}" >&2
    return 1
}

link_dpdk_bind_kernel() {
    local pci="$1"
    local driver

    if [[ ! -x "${DEVBIND}" ]]; then
        echo "[FAIL] 未找到 ${DEVBIND}" >&2
        return 1
    fi

    driver="$(link_dpdk_pci_driver "${pci}" || true)"
    if [[ "${driver}" == "${DPDK_KERNEL_DRIVER}" ]]; then
        echo "[OK]  ${pci} 已是 ${DPDK_KERNEL_DRIVER}"
        return 0
    fi

    echo "[*] dpdk-devbind: ${pci} → ${DPDK_KERNEL_DRIVER}"
    "${DEVBIND}" -b "${DPDK_KERNEL_DRIVER}" "${pci}" 2>/dev/null || \
        "${DEVBIND}" --force -b "${DPDK_KERNEL_DRIVER}" "${pci}"
    driver="$(link_dpdk_pci_driver "${pci}" || true)"
    if [[ "${driver}" == "${DPDK_KERNEL_DRIVER}" ]]; then
        echo "[OK]  ${pci} 已还回 ${DPDK_KERNEL_DRIVER}"
        return 0
    fi

    echo "[FAIL] ${DPDK_KERNEL_DRIVER} 还回失败: ${pci}" >&2
    return 1
}

# 同 IOMMU group 内 f0/f1 一并交给 DPDK 驱动，避免残留 ixgbe 干扰 probe。
link_dpdk_bind_iommu_group_uio() {
    echo "[*] IOMMU group: 绑定同组口 ${LINK_SIBLING_PCI}（${LINK_SIBLING_IFACE}）+ 目标口 ${LINK_PCI} → ${DPDK_BIND_DRIVER}"
    link_dpdk_bind_uio "${LINK_SIBLING_PCI}" "${LINK_SIBLING_IFACE}"
    link_dpdk_bind_uio "${LINK_PCI}"
}

link_dpdk_release_iommu_group_kernel() {
    echo "[*] IOMMU group: 还回目标口 ${LINK_PCI} + 同组口 ${LINK_SIBLING_PCI} → ${DPDK_KERNEL_DRIVER}"
    link_dpdk_bind_kernel "${LINK_PCI}"
    link_dpdk_bind_kernel "${LINK_SIBLING_PCI}"
}

link_profile_summary() {
    cat <<EOF
[*] link profile: ${LINK_NAME}
    iface        : ${LINK_IFACE}
    pci          : ${LINK_PCI}
    recv ipv6    : ${LINK_IPV6}/${LINK_PREFIX}
    remote ipv6  : ${LINK_REMOTE_IPV6}
    udp port     : ${LINK_DST_PORT}
    recv mac     : ${LINK_MAC}
    fstack cfg   : ${FSTACK_CONFIG}
EOF
}
