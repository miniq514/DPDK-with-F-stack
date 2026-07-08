#!/usr/bin/env bash
# setup_f1_ipv6.sh — dac 链路 f1 口 IPv6 绑定（8007 收端 / 8004 发端 二选一）
#
# 用法:
#   sudo ./test/setup_f1_ipv6.sh 8007
#   sudo ./test/setup_f1_ipv6.sh 8004
#   sudo ./test/setup_f1_ipv6.sh --role 8007
#
# 地址规划（与 docs/README.md · 1.0.1 对齐）:
#   8007 f1 (收端): 1111:2222:1111:2222:1111:2222:1111:0002/64
#   8004 f1 (发端): 1111:2222:1111:2222:1111:2222:1111:0003/64

set -euo pipefail

F1_NIC="enp66s0f1"
F1_PCI="0000:42:00.1"
IP6_PREFIX="64"
IP6_NET_PREFIX="1111:2222:1111:2222:1111:2222:1111"
IP6_8007="${IP6_NET_PREFIX}:0002"
IP6_8004="${IP6_NET_PREFIX}:0003"

ROLE=""

usage() {
    cat <<EOF
用法: $(basename "$0") <8007|8004>
      $(basename "$0") --role <8007|8004>
      $(basename "$0") --quiet <8007|8004>

为 dac 万兆口 ${F1_NIC} (${F1_PCI}) 配置 IPv6 并打印网卡状态。
8007 与 8004 二选一，便于在本机或对端服务器上运行同一脚本。

地址:
  8007 (收端): ${IP6_8007}/${IP6_PREFIX}
  8004 (发端): ${IP6_8004}/${IP6_PREFIX}

示例:
  sudo $0 8007
  sudo $0 --role 8004
EOF
}

log_step() { echo "==> $*"; }
log_info() { echo "    $*"; }

need_root() {
    if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
        echo "错误: 需要 root 权限，请使用 sudo 运行" >&2
        exit 1
    fi
}

QUIET=0

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -h|--help)
                usage
                exit 0
                ;;
            --role)
                ROLE="${2:-}"
                shift 2
                ;;
            --quiet)
                QUIET=1
                shift
                ;;
            8007|8004)
                ROLE="$1"
                shift
                ;;
            "")
                echo "错误: 缺少参数 <8007|8004>" >&2
                usage >&2
                exit 1
                ;;
            *)
                echo "错误: 未知参数 '$1'（仅支持 8007 或 8004）" >&2
                usage >&2
                exit 1
                ;;
        esac
    done

    if [[ "$ROLE" != "8007" && "$ROLE" != "8004" ]]; then
        echo "错误: role 必须是 8007 或 8004，当前: '${ROLE:-<empty>}'" >&2
        exit 1
    fi
}

role_ip6() {
    case "$ROLE" in
        8007) echo "$IP6_8007" ;;
        8004) echo "$IP6_8004" ;;
    esac
}

peer_ip6() {
    case "$ROLE" in
        8007) echo "$IP6_8004" ;;
        8004) echo "$IP6_8007" ;;
    esac
}

peer_role() {
    case "$ROLE" in
        8007) echo "8004" ;;
        8004) echo "8007" ;;
    esac
}

nic_exists() {
    [[ -d "/sys/class/net/${F1_NIC}" ]]
}

nic_driver() {
    if [[ -L "/sys/class/net/${F1_NIC}/device/driver" ]]; then
        basename "$(readlink -f "/sys/class/net/${F1_NIC}/device/driver")"
    else
        echo "unknown"
    fi
}

nic_operstate() {
    cat "/sys/class/net/${F1_NIC}/operstate" 2>/dev/null || echo "unknown"
}

nic_carrier() {
    if [[ -r "/sys/class/net/${F1_NIC}/carrier" ]]; then
        local c
        c="$(cat "/sys/class/net/${F1_NIC}/carrier")"
        if [[ "$c" == "1" ]]; then
            echo "up (Link detected: yes)"
        else
            echo "down (Link detected: no)"
        fi
    else
        echo "unknown (no carrier sysfs)"
    fi
}

nic_mac() {
    ip link show dev "$F1_NIC" 2>/dev/null \
        | awk '/link\/ether/ {print $2; exit}'
}

nic_ipv6_addrs() {
    ip -6 addr show dev "$F1_NIC" scope global 2>/dev/null \
        | awk '/inet6/ {print $2}' || true
}

remove_legacy_ipv4() {
    local nic="$1"
    local old
    while IFS= read -r old; do
        [[ -z "$old" ]] && continue
        log_step "Removing legacy IPv4 ${old} on ${nic}"
        ip addr del "$old" dev "$nic" 2>/dev/null || true
    done < <(ip -4 addr show dev "$nic" 2>/dev/null | awk '/inet / {print $2}')
}

remove_lab_ipv6() {
    local nic="$1"
    local addr
    while IFS= read -r addr; do
        [[ -z "$addr" ]] && continue
        case "$addr" in
            ${IP6_NET_PREFIX}:*)
                log_step "Removing lab IPv6 ${addr} on ${nic}"
                ip -6 addr del "$addr" dev "$nic" 2>/dev/null || true
                ;;
        esac
    done < <(ip -6 addr show dev "$nic" scope global 2>/dev/null | awk '/inet6/ {print $2}')
}

setup_f1_ipv6() {
    local target_ip6 peer
    target_ip6="$(role_ip6)"
    peer="$(peer_ip6)"

    if ! nic_exists; then
        echo "错误: 未找到网卡 ${F1_NIC}，请确认本机 DAC 口名称" >&2
        exit 1
    fi

    local driver
    driver="$(nic_driver)"
    if [[ "$driver" == "vfio-pci" ]]; then
        echo "错误: ${F1_NIC} 当前由 vfio-pci 占用（DPDK/F-Stack 可能正在运行）" >&2
        echo "      请先 release 网卡到 ixgbe 后再配置 IP" >&2
        exit 1
    fi

    [[ "$QUIET" -eq 0 ]] && log_step "Bringing up ${F1_NIC}"
    ip link set dev "$F1_NIC" up

    remove_legacy_ipv4 "$F1_NIC"
    remove_lab_ipv6 "$F1_NIC"

    [[ "$QUIET" -eq 0 ]] && log_step "Adding IPv6 ${target_ip6}/${IP6_PREFIX} on ${F1_NIC}"
    ip -6 addr add "${target_ip6}/${IP6_PREFIX}" dev "$F1_NIC"

    if [[ "$QUIET" -eq 0 ]]; then
        echo ""
        print_summary "$target_ip6" "$peer"
    fi
}

print_summary() {
    local target_ip6="$1" peer_ip6="$2"
    local driver operstate carrier mac addrs

    driver="$(nic_driver)"
    operstate="$(nic_operstate)"
    carrier="$(nic_carrier)"
    mac="$(nic_mac)"
    addrs="$(nic_ipv6_addrs)"

    echo "========================================"
    echo "  f1 DAC 网卡信息 (${ROLE})"
    echo "========================================"
    echo "  角色            : ${ROLE} ($( [[ "$ROLE" == "8007" ]] && echo '收端' || echo '发端' ))"
    echo "  接口            : ${F1_NIC}"
    echo "  PCI             : ${F1_PCI}"
    echo "  MAC             : ${mac:-unknown}"
    echo "  驱动            : ${driver}"
    echo "  operstate       : ${operstate}"
    echo "  链路            : ${carrier}"
    echo "  本机 IPv6       : ${target_ip6}/${IP6_PREFIX}"
    echo "  对端 IPv6       : ${peer_ip6}/${IP6_PREFIX} ($(peer_role))"
    echo "  全局 IPv6 列表  :"
    if [[ -n "$addrs" ]]; then
        while IFS= read -r a; do
            echo "                    ${a}"
        done <<< "$addrs"
    else
        echo "                    (none)"
    fi
    echo "========================================"

    if [[ "$carrier" == *"no"* ]]; then
        echo ""
        echo "提示: 链路未 up，请检查 DAC 铜缆与对端 $(peer_role) 是否已配置并 up。"
    else
        echo ""
        echo "连通性测试（可选）:"
        echo "  ping6 -I ${F1_NIC} -c 3 ${peer_ip6}"
    fi
}

main() {
    parse_args "$@"
    need_root
    setup_f1_ipv6
}

main "$@"
