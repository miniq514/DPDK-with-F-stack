#!/usr/bin/env bash
#
# sender/run_pktgen_sender.sh — 程序B：pktgen 一键打流（发包数量可调）
#
# 用法:
#   sudo bash run_pktgen_sender.sh                                   # 冒烟 100 包 → 8007
#   sudo bash run_pktgen_sender.sh --to-8007 --count 2000000 --rate-mbps 6000
#   sudo bash run_pktgen_sender.sh --to-8004 --count 100000          # 反向/自环
#   sudo bash run_pktgen_sender.sh --setup-dpdk --pci 0000:42:00.1
#   sudo bash run_pktgen_sender.sh --check                           # 环境检查（不发包）
#
# 时序：收端先 READY，发端再发包。--count 须与收端 --expected-pkts 一致。
# 收端 DPDK 绑 vfio 后不回 ARP → pcap 内嵌收端 MAC（由 --to-* 自动选择）。
# 打流结束（含 Ctrl+C）后自动还网卡到 ixgbe 并恢复本机 IPv6（除非 --no-release）。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# sender/ root; hugepage.sh via ${BUNDLE_DIR}/../scripts/lib
BUNDLE_DIR="${SCRIPT_DIR}"

CONFIG_H="${SCRIPT_DIR}/include/sender_config.h"
GEN_BIN="${SCRIPT_DIR}/../build/sender/gen_udp_pcap"
REPLAY_SH="${SCRIPT_DIR}/scripts/run_pktgen_replay.sh"
# 若本脚本位于 Fstack-DPDk/sender/，setup_f1_ipv6.sh 在 ../test/internet/
SETUP_IPV6_SH="${SCRIPT_DIR}/../test/internet/setup_f1_ipv6.sh"
DEVBIND="${DEVBIND:-/usr/local/bin/dpdk-devbind.py}"

# ---------- 本机固定参数（与 sender_config.h / 收端对齐）----------
BIND_IF="${BIND_IF:-}"
TX_PCI="${TX_PCI:-}"
SSH_NIC_PCI="${SSH_NIC_PCI:-0000:44:00.0}"

# 目标：默认 8007
TARGET_8004=0
DST_IP=""
DST_MAC=""
SRC_IP=""
SRC_MAC=""
SRC_PORT="${SRC_PORT:-50001}"
DST_PORT="${DST_PORT:-8888}"
# 数据区字节；raw 模式下 UDP payload = 8B头 + PAYLOAD_SIZE体 = 1344B
PAYLOAD_SIZE="${PAYLOAD_SIZE:-1336}"
COUNT="${COUNT:-100}"
PCAP_COUNT="${PCAP_COUNT:-}"   # pcap 模板包数；默认 100000（pktgen 循环至 --count）
PCAP_TEMPLATE_MAX=5000
PCAP="${PCAP:-${SCRIPT_DIR}/udp_test.pcap}"

RATE=""           # 线路速率百分比；空 = 100（线速）
RATE_MBPS=""
LINE_GBPS="${LINE_GBPS:-10}"
DURATION=""
FORCE_REGEN=0
SKIP_REGEN=0
NO_RELEASE=0
SENDER_RELEASE_DONE=0
MODE="send"

usage() {
    cat <<EOF
Usage: sudo bash run_pktgen_sender.sh [options]

发端 pktgen 一键打流（IPv6，pcap 重放）。

目标切换（dst IP / dst MAC 自动选择）:
  --to-8007           打流目标 8007（默认）：dst …:0002 / mac 90:e2:ba:50:aa:71
  --to-8004           打流目标 8004：dst …:0003 / mac 90:e2:ba:50:7c:f9（反向/自环）

收端先 READY，例如（8007）:
  sudo ./scripts/run_recv.sh --link dac --raw-payload --expected-pkts ${COUNT} \
      --output test/output/live_dac.bin

Options:
  --to-8007 / --to-8004   切换打流目标（见上）
  --setup-dpdk            绑 vfio-pci + 配置 hugepages（首次部署）
  --check                 仅环境检查（hugepages / vfio / pcap MAC）
  --gen-pcap              仅生成 pcap，不发包
  --force-regen-pcap      强制重新生成 pcap（即使已存在）
  --no-regen-pcap         不自动生成 pcap（必须已存在）
  --no-release            打流结束后不自动还网卡/恢复 IPv6

  --pci ADDR              发端万兆口 PCI（也可用 TX_PCI；默认从 BIND_IF 解析）
  --bind-if NAME          发端万兆口名（绑 vfio 前，default enp66s0f1）
  --count N               包数 (default ${COUNT})
  --payload-size B        数据区字节 (default ${PAYLOAD_SIZE}；raw 下 UDP payload = 8B头+${PAYLOAD_SIZE}B体)
  --rate PERCENT          线路速率百分比 0.01..100（默认 100 = 线速）
  --rate-mbps MBPS        目标线路带宽 Mbps（与 --rate 二选一）
  --duration SEC          按时长发包（与 --count 二选一）
  --line-gbps N           网口标称线速，供 --rate-mbps 换算 (default ${LINE_GBPS})
  --pcap FILE             pcap 路径 (default ${PCAP})
  --dst-mac MAC           收端万兆口 MAC（覆盖 --to-* 默认）
  --src-ip IP6            源 IPv6（覆盖 --to-* 反推默认）
  --src-mac MAC           源 MAC（覆盖 --to-* 反推默认）
  --src-port PORT         源 UDP 端口 (default ${SRC_PORT})
  --dst-port PORT         目的 UDP 端口 (default ${DST_PORT})
  -h, --help              显示帮助

环境变量: BIND_IF TX_PCI SRC_IP DST_IP DST_MAC COUNT PAYLOAD_SIZE RATE_MBPS

示例:
  sudo bash run_pktgen_sender.sh
  sudo bash run_pktgen_sender.sh --to-8007 --count 2000000 --rate-mbps 6000
  sudo bash run_pktgen_sender.sh --to-8004 --count 100000 --rate 100
  sudo bash run_pktgen_sender.sh --setup-dpdk --pci 0000:42:00.1
EOF
}

log()  { echo "[*] $*"; }
warn() { echo "[!] $*" >&2; }
die()  { echo "[x] $*" >&2; exit 1; }

require_root() {
    if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
        die "请使用 sudo 运行（需要 DPDK / pktgen）"
    fi
}

read_config() {
    local key="$1"
    [[ -f "${CONFIG_H}" ]] || return 1
    sed -n "s/^#define ${key}[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p" "${CONFIG_H}" | head -1
}

# 按 --to-* 与覆盖参数解析最终 dst/src IP/MAC
resolve_target_addrs() {
    if [[ -z "${DST_IP}" ]]; then
        if [[ "${TARGET_8004}" -eq 1 ]]; then
            DST_IP="$(read_config SEND_TARGET_8004_IP6 || echo "1111:2222:1111:2222:1111:2222:1111:0003")"
        else
            DST_IP="$(read_config SEND_TARGET_8007_IP6 || echo "1111:2222:1111:2222:1111:2222:1111:0002")"
        fi
    fi
    if [[ -z "${DST_MAC}" ]]; then
        if [[ "${TARGET_8004}" -eq 1 ]]; then
            DST_MAC="$(read_config SEND_TARGET_8004_MAC || echo "90:e2:ba:50:7c:f9")"
        else
            DST_MAC="$(read_config SEND_TARGET_8007_MAC || echo "90:e2:ba:50:aa:71")"
        fi
    fi
    if [[ -z "${SRC_IP}" ]]; then
        if [[ "${TARGET_8004}" -eq 1 ]]; then
            SRC_IP="$(read_config SEND_SRC_8007_IP6 || echo "1111:2222:1111:2222:1111:2222:1111:0002")"
        else
            SRC_IP="$(read_config SEND_SRC_8004_IP6 || echo "1111:2222:1111:2222:1111:2222:1111:0003")"
        fi
    fi
    if [[ -z "${SRC_MAC}" ]]; then
        if [[ "${TARGET_8004}" -eq 1 ]]; then
            SRC_MAC="$(read_config SEND_SRC_8007_MAC || echo "90:e2:ba:50:aa:71")"
        else
            SRC_MAC="$(read_config SEND_SRC_8004_MAC || echo "90:e2:ba:50:7c:f9")"
        fi
    fi
    if [[ -z "${TX_PCI}" ]]; then
        TX_PCI="$(read_config SEND_TX_PCI_DEFAULT || true)"
    fi
    if [[ -z "${BIND_IF}" ]]; then
        BIND_IF="$(read_config SEND_TX_IF_DEFAULT || true)"
    fi
}

# --to-8007 → 本机 8004；--to-8004 → 本机 8007（与 setup_f1_ipv6.sh 角色参数一致）
resolve_local_role() {
    if [[ "${TARGET_8004}" -eq 1 ]]; then
        echo "8007"
    else
        echo "8004"
    fi
}

# 打流前：内核态配网 + 链路摘要（须在绑 vfio 之前，此时 carrier 才准确）
prepare_sender_link() {
    local role pci
    role="$(resolve_local_role)"
    pci="$(resolve_tx_pci)"

    if [[ -z "${pci}" ]]; then
        warn "无法解析 PCI，跳过打流前链路检查"
        return 0
    fi

    if is_vfio_bound "${pci}"; then
        warn "发包口 ${pci} 已是 vfio-pci，跳过内核态链路检查"
        return 0
    fi

    if [[ ! -f "${SETUP_IPV6_SH}" ]]; then
        warn "未找到 ${SETUP_IPV6_SH}，跳过打流前链路检查"
        return 0
    fi

    echo ""
    echo "---- 打流前网卡/链路检查（内核态，绑 vfio 前）----"
    bash "${SETUP_IPV6_SH}" "${role}" || warn "打流前配网/链路检查失败"
}

release_sender_env() {
    if [[ "${NO_RELEASE}" -eq 1 || "${SENDER_RELEASE_DONE}" -eq 1 ]]; then
        return 0
    fi
    SENDER_RELEASE_DONE=1

    local pci role ifname
    pci="$(resolve_tx_pci)"
    role="$(resolve_local_role)"
    ifname="${BIND_IF:-$(read_config SEND_TX_IF_DEFAULT || echo enp66s0f1)}"

    if [[ -z "${pci}" ]]; then
        warn "release: 无法解析 PCI，跳过还网卡"
        return 0
    fi

    log "发端 release: ${pci} → ixgbe，恢复本机 IPv6（角色 ${role}）"

    if [[ -x "${DEVBIND}" ]]; then
        if "${DEVBIND}" -s 2>/dev/null | grep -i "${pci}" | grep -qi "drv=vfio-pci"; then
            "${DEVBIND}" -b ixgbe "${pci}" 2>/dev/null || \
                "${DEVBIND}" --force -b ixgbe "${pci}" || warn "ixgbe 还回失败: ${pci}"
        else
            log "PCI ${pci} 已是 ixgbe，跳过 devbind"
        fi
    else
        warn "未找到 ${DEVBIND}，跳过还驱动"
    fi

    for _ in 1 2 3 4 5; do
        if ip link show "${ifname}" &>/dev/null; then
            break
        fi
        sleep 0.5
    done

    if [[ -f "${SETUP_IPV6_SH}" ]]; then
        if bash "${SETUP_IPV6_SH}" --quiet "${role}"; then
            log "release 完成（${ifname} 已还回 ixgbe，IPv6 已恢复）"
        else
            warn "setup_f1_ipv6.sh --quiet ${role} 失败，请手动: sudo bash ${SETUP_IPV6_SH} ${role}"
        fi
    else
        warn "未找到 ${SETUP_IPV6_SH}，请手动恢复 IPv6"
    fi
}

# Ctrl+C / SIGTERM：release 后必须 exit，否则 read 被中断后会继续等待 Enter
on_send_interrupt() {
    echo "" >&2
    release_sender_env
    trap - EXIT INT TERM
    exit 130
}

clear_send_traps() {
    trap - EXIT INT TERM
}

resolve_pci_from_if() {
    local ifname="$1"
    local dev
    dev="$(readlink -f "/sys/class/net/${ifname}/device" 2>/dev/null || true)"
    [[ -n "${dev}" ]] || return 1
    basename "${dev}"
}

resolve_tx_pci() {
    if [[ -n "${TX_PCI}" ]]; then
        echo "${TX_PCI}"
        return 0
    fi
    if [[ -n "${BIND_IF}" ]] && ip link show "${BIND_IF}" &>/dev/null; then
        resolve_pci_from_if "${BIND_IF}" || true
    fi
}

ensure_build() {
    if [[ -x "${GEN_BIN}" ]]; then
        return 0
    fi
    log "编译 gen_udp_pcap ..."
    # 优先走项目根 CMake（add_subdirectory(sender)）；否则本目录独立 cmake
    local root="${SCRIPT_DIR}/.."
    if [[ -f "${root}/CMakeLists.txt" ]] && command -v cmake &>/dev/null; then
        cmake -S "${root}" -B "${root}/build" >/dev/null
        cmake --build "${root}/build" --target gen_udp_pcap -j"$(nproc)" >/dev/null
    elif command -v cmake &>/dev/null && [[ -f "${SCRIPT_DIR}/CMakeLists.txt" ]]; then
        cmake -S "${SCRIPT_DIR}" -B "${SCRIPT_DIR}/build" >/dev/null
        cmake --build "${SCRIPT_DIR}/build" -j"$(nproc)" >/dev/null
    else
        die "无 cmake，无法编译 gen_udp_pcap"
    fi
    [[ -x "${GEN_BIN}" ]] || die "gen_udp_pcap 编译失败: ${GEN_BIN}"
}

count_pcap_packets() {
    python3 - "$1" <<'PY'
import struct, sys
path = sys.argv[1]
with open(path, "rb") as f:
    magic = f.read(4)
    if magic not in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4"):
        sys.exit(2)
    f.read(20)
    n = 0
    while True:
        hdr = f.read(16)
        if len(hdr) < 16:
            break
        incl = struct.unpack("<I", hdr[8:12])[0]
        f.read(incl)
        n += 1
print(n)
PY
}

ensure_pcap() {
    if [[ "${SKIP_REGEN}" -eq 1 ]]; then
        [[ -f "${PCAP}" ]] || die "pcap 不存在: ${PCAP}（去掉 --no-regen-pcap 或先 --gen-pcap）"
        return 0
    fi
    if [[ -f "${PCAP}" && "${FORCE_REGEN}" -eq 0 ]]; then
        local existing
        existing="$(count_pcap_packets "${PCAP}" 2>/dev/null || echo 0)"
        if [[ "${existing}" -gt "${PCAP_TEMPLATE_MAX}" ]]; then
            die "pcap 含 ${existing} 包，pktgen 无法加载（上限 ${PCAP_TEMPLATE_MAX}）；请加 --force-regen-pcap 重建"
        fi
        log "使用已有 pcap: ${PCAP} (${existing} 包)"
        return 0
    fi
    [[ -n "${DST_MAC}" ]] || die "未指定收端 MAC（解析 --to-* 或 --dst-mac 失败）"

    # pktgen 启动时会加载整个 pcap 到内存，不可生成 200 万帧；用模板循环
    local gen_count="${PCAP_COUNT:-${PCAP_TEMPLATE_MAX}}"
    if [[ -n "${COUNT}" && "${COUNT}" -le "${gen_count}" ]]; then
        gen_count="${COUNT}"
    fi
    if [[ -n "${COUNT}" && "${COUNT}" -gt "${gen_count}" ]]; then
        log "pcap 模板 ${gen_count} 包，pktgen 循环发送至 ${COUNT} 包"
    fi

    ensure_build
    local to_arg
    if [[ "${TARGET_8004}" -eq 1 ]]; then to_arg="--to-8004"; else to_arg="--to-8007"; fi
    log "生成 IPv6 pcap: ${PCAP} (${gen_count} x $((PAYLOAD_SIZE + 8)) B UDP payload = 8B头+${PAYLOAD_SIZE}B体, ${to_arg}, dst ${DST_IP}:${DST_PORT}, mac ${DST_MAC})"
    "${GEN_BIN}" "${PCAP}" \
        ${to_arg} \
        --count "${gen_count}" \
        --payload-size "${PAYLOAD_SIZE}" \
        --src-ip "${SRC_IP}" \
        --dst-ip "${DST_IP}" \
        --src-port "${SRC_PORT}" \
        --dst-port "${DST_PORT}" \
        --src-mac "${SRC_MAC}" \
        --dst-mac "${DST_MAC}"
}

prepare_nic_for_dpdk() {
    local pci="$1"
    local ifname="${BIND_IF:-}"

    if [[ -n "${ifname}" ]] && ip link show "${ifname}" &>/dev/null; then
        log "释放内核网口 ${ifname}（pktgen 走 DPDK 裸帧，不需要内核 IP）"
        ip link set "${ifname}" down 2>/dev/null || true
        ip addr flush dev "${ifname}" 2>/dev/null || true
    fi

    modprobe vfio-pci 2>/dev/null || true
}

is_vfio_bound() {
    local pci="$1"
    [[ -x "${DEVBIND}" ]] || return 1
    "${DEVBIND}" -s 2>/dev/null | grep -i "${pci}" | grep -qi "drv=vfio-pci"
}

bind_vfio_pci() {
    local pci="$1"
    prepare_nic_for_dpdk "${pci}"

    if is_vfio_bound "${pci}"; then
        log "PCI ${pci} 已是 vfio-pci"
        return 0
    fi

    [[ -x "${DEVBIND}" ]] || die "未找到 ${DEVBIND}"
    warn "绑定 vfio-pci: ${pci} — 确认不是 SSH 口!"
    if ! "${DEVBIND}" --force -b vfio-pci "${pci}" 2>&1; then
        die "vfio-pci 绑定失败: ${pci}"
    fi
    is_vfio_bound "${pci}" || die "vfio-pci 绑定后仍未生效: ${pci}（检查路由/占用）"
    log "PCI ${pci} 已绑 vfio-pci"
}

hugepages_2m_count() {
    # 兼容旧调用：返回本机 2MB 大页总数（继承 /sys 配置）
    # shellcheck source=../../scripts/lib/hugepage.sh
    source "${BUNDLE_DIR}/../scripts/lib/hugepage.sh"
    hugepages_2m_total
}

cleanup_stale_hugepages() {
    # shellcheck source=../../scripts/lib/hugepage.sh
    source "${BUNDLE_DIR}/../scripts/lib/hugepage.sh"
    HUGEPAGE_FILE_PREFIX="udp_pktgen_tx"
    hugepages_cleanup_project_maps "${HUGEPAGE_FILE_PREFIX}"
}

setup_hugepages() {
    # shellcheck source=../../scripts/lib/hugepage.sh
    source "${BUNDLE_DIR}/../scripts/lib/hugepage.sh"
    HUGEPAGE_FILE_PREFIX="udp_pktgen_tx"
    HUGEPAGE_MIN_TOTAL=512
    HUGEPAGE_MIN_FREE=256
    HUGEPAGE_EXPAND_TARGET="4G"
    hugepages_ensure_inherit
}

setup_dpdk() {
    require_root
    local pci
    pci="$(resolve_tx_pci)"
    [[ -n "${pci}" ]] || die "无法解析发端 PCI，请指定 --pci 或 TX_PCI，或设置 BIND_IF=${BIND_IF}"

    if [[ "${pci}" == "${SSH_NIC_PCI}" ]]; then
        die "拒绝绑定 SSH/管理口 PCI ${pci}"
    fi

    log "DPDK 发端初始化: PCI=${pci} (if=${BIND_IF:-<unknown>})"
    bind_vfio_pci "${pci}"
    setup_hugepages || warn "hugepages 未完全达标，若 pktgen 启动失败请先清理 /dev/hugepages"
    log "DPDK 发端环境就绪（vfio 已绑 ${pci}）"
}

run_check() {
    require_root
    resolve_target_addrs
    local pci
    pci="$(resolve_tx_pci)"
    [[ -n "${pci}" ]] || die "无法解析发端 PCI，请 --pci 或 TX_PCI 或 BIND_IF"

    local args=(--check-only --pci "${pci}")
    [[ -f "${PCAP}" ]] && args+=(--pcap "${PCAP}")
    [[ -n "${DST_MAC}" ]] && args+=(--dst-mac "${DST_MAC}")
    bash "${REPLAY_SH}" "${args[@]}"
}

run_send() {
    require_root
    resolve_target_addrs
    ensure_pcap

    local pci
    pci="$(resolve_tx_pci)"
    [[ -n "${pci}" ]] || die "无法解析发端 PCI；pktgen 需 vfio 绑定后 PCI，请 --pci 或 TX_PCI"

    if [[ "${pci}" == "${SSH_NIC_PCI}" ]]; then
        die "拒绝在 SSH/管理口 ${pci} 上发包"
    fi

    prepare_sender_link

    if ! is_vfio_bound "${pci}"; then
        warn "发包口 ${pci} 未绑 vfio-pci，尝试自动初始化 ..."
        bind_vfio_pci "${pci}"
        setup_hugepages
    fi

    trap release_sender_env EXIT
    trap on_send_interrupt INT TERM

    echo ""
    # SSH/sshpass: skip prompt when non-interactive
    if [[ "${NO_PROMPT:-0}" == "1" ]] || ! [[ -t 0 ]]; then
        echo "[*] skip prompt (non-interactive / NO_PROMPT=1)"
    else
        read -r -p "收端就绪后按 Enter 开始 pktgen，Ctrl+C 取消 ... " _ || on_send_interrupt
    fi

    local args=(
        --pcap "${PCAP}"
        --pci "${pci}"
        --line-gbps "${LINE_GBPS}"
    )
    [[ -n "${DST_MAC}" ]] && args+=(--dst-mac "${DST_MAC}")
    if [[ -n "${COUNT}" && -z "${DURATION}" ]]; then
        args+=(--count "${COUNT}")
    elif [[ -n "${DURATION}" ]]; then
        args+=(--duration "${DURATION}")
    fi
    if [[ -n "${RATE_MBPS}" ]]; then
        args+=(--rate-mbps "${RATE_MBPS}")
    elif [[ -n "${RATE}" ]]; then
        args+=(--rate "${RATE}")
    fi

    log "执行: bash scripts/run_pktgen_replay.sh ${args[*]}"
    bash "${REPLAY_SH}" "${args[@]}"
    local rc=$?
    clear_send_traps
    release_sender_env
    return "${rc}"
}

# ---------- 解析参数 ----------
EXTRA=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --to-8007)         TARGET_8004=0; shift ;;
        --to-8004)         TARGET_8004=1; shift ;;
        --setup-dpdk)      MODE="setup"; shift ;;
        --check)           MODE="check"; shift ;;
        --gen-pcap)        MODE="gen"; shift ;;
        --force-regen-pcap) FORCE_REGEN=1; shift ;;
        --no-regen-pcap)   SKIP_REGEN=1; shift ;;
        --no-release)      NO_RELEASE=1; shift ;;
        --pci)             TX_PCI="$2"; shift 2 ;;
        --bind-if)         BIND_IF="$2"; shift 2 ;;
        --count)           COUNT="$2"; shift 2 ;;
        --payload-size)    PAYLOAD_SIZE="$2"; shift 2 ;;
        --rate)            RATE="$2"; RATE_MBPS=""; shift 2 ;;
        --rate-mbps)       RATE_MBPS="$2"; RATE=""; shift 2 ;;
        --duration)        DURATION="$2"; COUNT=""; shift 2 ;;
        --line-gbps)       LINE_GBPS="$2"; shift 2 ;;
        --pcap)            PCAP="$2"; shift 2 ;;
        --dst-mac)         DST_MAC="$2"; shift 2 ;;
        --src-mac)         SRC_MAC="$2"; shift 2 ;;
        --src-ip)          SRC_IP="$2"; shift 2 ;;
        --dst-ip)          DST_IP="$2"; shift 2 ;;
        --src-port)        SRC_PORT="$2"; shift 2 ;;
        --dst-port)        DST_PORT="$2"; shift 2 ;;
        -h|--help)         usage; exit 0 ;;
        --)                shift; EXTRA+=("$@"); break ;;
        *)                 die "未知参数: $1（-h 查看帮助）" ;;
    esac
done

resolve_target_addrs

case "${MODE}" in
    setup)  setup_dpdk ;;
    check)  run_check ;;
    gen)
        ensure_pcap
        log "pcap 已生成: ${PCAP}"
        ;;
    send)   run_send ;;
esac
