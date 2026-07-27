#!/usr/bin/env bash
# sender/scripts/run_pktgen_replay.sh
#
# 发端用 DPDK pktgen 重放 IPv6 pcap 发流（程序B 底层编排）。
# pcap 由 ../gen_udp_pcap 生成（IPv6 UDP），格式与收端 fstack_udp_recv / udp_payload.h 一致。
# pktgen 只重放以太网帧，不解析 L3，IPv6 pcap 兼容性无需改 pktgen 核心。
#
# 用法:
#   sudo bash scripts/run_pktgen_replay.sh --pcap udp_test.pcap --pci <发端PCI> [options]
#
# 速率控制（二选一，最终都转为 pktgen 的线路速率百分比）:
#   --rate PERCENT       线路速率百分比 0.01..100（默认 100 = 线速）
#   --rate-mbps MBPS     目标线路带宽（Mbps），按 --line-gbps 换算为百分比
#                        例: 10G 口限速 6000 Mbps → --rate-mbps 6000

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUNDLE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIG_H="${BUNDLE_DIR}/include/sender_config.h"

PKTGEN="${PKTGEN:-/usr/local/bin/pktgen}"
DEVBIND="${DEVBIND:-/usr/local/bin/dpdk-devbind.py}"

PCAP=""
PCI=""
RATE=""               # 空 = 未显式指定 percent
RATE_MBPS=""
LINE_GBPS=10
DURATION=10
COUNT=""
# pktgen pcap 循环 + 线速突发会少发尾部；按包数留足余量，收端仍用原始 --count
PKTGEN_SEND_MARGIN="${PKTGEN_SEND_MARGIN:-}"
LCORES="0,1,2"
CHANNELS=4
FILE_PREFIX="udp_pktgen_tx"
# pktgen 无法一次加载超大 pcap；模板包数，实际发送数由 set 0 count 控制
PCAP_TEMPLATE_MAX=5000
CHECK_ONLY=0
EXPECTED_DST_MAC="${DST_MAC:-}"

usage() {
    sed -n '2,20p' "$0"
    echo ""
    echo "速率换算（10G 口、1344B payload 帧约 1400B）:"
    echo "  --rate-mbps 6000  ≈ 60% 线路 ≈ 6 Gbps wire"
    echo "  --rate-mbps 1000  ≈ 10% 线路 ≈ 1 Gbps wire"
    echo "  --rate 100        = 100% 线速（约 10 Gbps）"
    echo ""
    echo "DPDK 收端目的 MAC（pcap 内嵌，等价于内核静态 ARP）:"
    echo "  --dst-mac MAC     校验 pcap 首包目的 MAC（默认读 sender_config.h 或 DST_MAC）"
    echo "  DST_MAC=..        同上，环境变量"
}

read_config_mac() {
    local key="$1"
    if [[ ! -f "${CONFIG_H}" ]]; then
        return 1
    fi
    sed -n "s/^#define ${key}[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p" "${CONFIG_H}" | head -1
}

normalize_mac() {
    local mac="${1,,}"
    mac="${mac//-/:}"
    if [[ ! "$mac" =~ ^([0-9a-f]{2}:){5}[0-9a-f]{2}$ ]]; then
        echo "[FAIL] 非法 MAC: $1" >&2
        return 1
    fi
    echo "$mac"
}

read_pcap_dst_mac() {
    local pcap="$1"
    python3 - "$pcap" <<'PY'
import sys
path = sys.argv[1]
with open(path, "rb") as f:
    magic = f.read(4)
    if magic not in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4"):
        sys.exit(2)
    f.read(20)
    f.read(16)
    dst = f.read(6)
    if len(dst) != 6:
        sys.exit(3)
    print(":".join(f"{b:02x}" for b in dst))
PY
}

check_pcap_dst_mac() {
    local pcap="$1"
    local expect="$2"
    local actual

    actual="$(read_pcap_dst_mac "${pcap}" 2>/dev/null || true)"
    if [[ -z "${actual}" ]]; then
        echo "[WARN] 无法解析 pcap 目的 MAC，请确认 gen_udp_pcap 已指定 --dst-mac" >&2
        return 0
    fi
    actual="$(normalize_mac "${actual}")" || return 1
    echo "[*]  pcap 首包目的 MAC: ${actual}"
    if [[ -n "${expect}" ]]; then
        expect="$(normalize_mac "${expect}")" || return 1
        if [[ "${actual}" != "${expect}" ]]; then
            echo "[FAIL] pcap 目的 MAC (${actual}) 与期望 (${expect}) 不一致" >&2
            echo "       重新生成: ./build/gen_udp_pcap ... --to-8007/--to-8004" >&2
            return 1
        fi
        echo "[OK]  pcap 目的 MAC 与目标一致"
    fi
    return 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pcap)         PCAP="$2";        shift 2 ;;
        --pci)          PCI="$2";         shift 2 ;;
        --rate)         RATE="$2";        shift 2 ;;
        --rate-mbps)    RATE_MBPS="$2";   shift 2 ;;
        --line-gbps)    LINE_GBPS="$2";   shift 2 ;;
        --duration)     DURATION="$2";    shift 2 ;;
        --count)        COUNT="$2";       shift 2 ;;
        --lcores)       LCORES="$2";      shift 2 ;;
        --channels)     CHANNELS="$2";    shift 2 ;;
        --file-prefix)  FILE_PREFIX="$2"; shift 2 ;;
        --check-only)   CHECK_ONLY=1;     shift ;;
        --dst-mac)      EXPECTED_DST_MAC="$2"; shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
    esac
done

# 解析最终 rate 百分比
if [[ -n "${RATE_MBPS}" ]]; then
    if [[ -n "${RATE}" ]]; then
        echo "[FAIL] --rate 与 --rate-mbps 不能同时使用" >&2
        exit 1
    fi
    RATE="$(awk -v mbps="${RATE_MBPS}" -v gbps="${LINE_GBPS}" \
        'BEGIN { if (gbps <= 0) exit 1; printf "%.4f", mbps / (gbps * 1000.0) * 100.0 }')"
    if [[ -z "${RATE}" ]]; then
        echo "[FAIL] --line-gbps 必须 > 0" >&2
        exit 1
    fi
elif [[ -z "${RATE}" ]]; then
    RATE=100
fi

if [[ -z "${PCI}" && "${CHECK_ONLY}" != "1" ]]; then
    echo "[FAIL] 必须指定发端发包网卡 --pci <ADDR>" >&2
    echo "       用 dpdk-devbind.py -s 查看 PCI；勿绑定 SSH/管理口" >&2
    exit 1
fi

echo "============================================================="
echo "  DPDK pktgen IPv6 pcap replay (发端)"
echo "============================================================="
echo "  bundle        : ${BUNDLE_DIR}"
echo "  pktgen        : ${PKTGEN}"
echo "  pcap          : ${PCAP:-<none>}"
echo "  TX port PCI   : ${PCI:-<check-only>}"
echo "  line rate     : ${LINE_GBPS} Gbps (for --rate-mbps conversion)"
if [[ -n "${RATE_MBPS}" ]]; then
    echo "  target        : ${RATE_MBPS} Mbps wire → ${RATE}% of line"
else
    echo "  rate          : ${RATE}% of line rate"
fi
if [[ -n "${COUNT}" ]]; then
    echo "  limit         : ${COUNT} packets"
else
    echo "  limit         : ${DURATION} seconds"
fi
echo "  lcores        : ${LCORES}, channels ${CHANNELS}"
echo "  file-prefix   : ${FILE_PREFIX}"
if [[ -n "${EXPECTED_DST_MAC}" ]]; then
    echo "  expect dst MAC: ${EXPECTED_DST_MAC}"
fi
echo "============================================================="

if [[ ! -x "${PKTGEN}" ]]; then
    echo "[FAIL] pktgen not found: ${PKTGEN}" >&2
    echo "       发端需安装 DPDK pktgen（通常 /usr/local/bin/pktgen）" >&2
    exit 1
fi

if [[ "${CHECK_ONLY}" == "1" ]]; then
    PCAP="${PCAP:-/tmp/nonexist.pcap}"
fi

if [[ -z "${PCAP}" || ! -f "${PCAP}" ]]; then
    if [[ "${CHECK_ONLY}" == "1" && "${PCAP}" == "/tmp/nonexist.pcap" ]]; then
        : # check-only 允许无 pcap
    else
        echo "[FAIL] pcap not found: ${PCAP}" >&2
        echo "       先运行: ./build/gen_udp_pcap udp_test.pcap --to-8007 --count N ..." >&2
        exit 1
    fi
fi

if [[ -f "${PCAP}" ]]; then
    echo "---- pcap 目的 MAC 检查 ----"
    check_pcap_dst_mac "${PCAP}" "${EXPECTED_DST_MAC}" || exit 1
fi

HP_FREE="$(awk '/HugePages_Free/{print $2}' /proc/meminfo 2>/dev/null || echo 0)"
if [[ "${HP_FREE}" -lt 512 ]]; then
    if command -v dpdk-hugepages.py &>/dev/null; then
        HP_DPDK="$(dpdk-hugepages.py --show 2>/dev/null | awk 'NR==2{print $2}' || echo 0)"
        if [[ "${HP_DPDK}" -ge 512 ]]; then
            echo "[OK]  hugepages (dpdk-hugepages): ${HP_DPDK} x 2MB"
        else
            echo "[WARN] HugePages_Free=${HP_FREE} (<512), pktgen 可能失败" >&2
            echo "       建议: sudo dpdk-hugepages.py -p 2M --setup 4G" >&2
        fi
    else
        echo "[WARN] HugePages_Free=${HP_FREE} (<512), pktgen 可能失败" >&2
        echo "       建议: sudo dpdk-hugepages.py -p 2M --setup 4G" >&2
    fi
else
    echo "[OK]  HugePages_Free=${HP_FREE}"
fi

DPDK_BIND_DRIVER="${DPDK_BIND_DRIVER:-}"

is_dpdk_bound() {
    local pci="$1"
    local line drv
    [[ -x "${DEVBIND}" ]] || return 1
    line="$("${DEVBIND}" -s 2>/dev/null | grep -i "${pci}" | head -1 || true)"
    [[ -n "${line}" ]] || return 1
    if [[ -n "${DPDK_BIND_DRIVER}" ]]; then
        echo "${line}" | grep -qi "drv=${DPDK_BIND_DRIVER}"
        return $?
    fi
    echo "${line}" | grep -Eqi 'drv=(vfio-pci|uio_pci_generic|igb_uio)'
}

if [[ -n "${PCI}" ]]; then
    echo "---- 网卡绑定状态 ----"
    if [[ -x "${DEVBIND}" ]]; then
        "${DEVBIND}" -s 2>/dev/null | grep -iE 'Network devices|drv=|unused=' || true
        if ! is_dpdk_bound "${PCI}"; then
            echo "[!] PCI ${PCI} 未绑 DPDK 驱动（pktgen 需要 vfio-pci / uio_pci_generic / igb_uio）"
            echo "    sudo modprobe uio_pci_generic  # 或 vfio-pci / igb_uio"
            echo "    sudo ${DEVBIND} -b uio_pci_generic ${PCI}"
            echo "    [!] 确认 ${PCI} 不是 SSH 口!"
            if [[ "${CHECK_ONLY}" != "1" ]]; then
                echo "[ABORT] 发包口未绑 DPDK 驱动" >&2
                exit 2
            fi
        else
            echo "[OK]  PCI ${PCI} 已绑 DPDK 驱动"
        fi
    else
        echo "[WARN] 未找到 ${DEVBIND}, 跳过绑定检查" >&2
    fi
fi

if [[ "${CHECK_ONLY}" == "1" ]]; then
    echo "[OK]  --check-only: 环境检查完成（未发包）"
    exit 0
fi

# 按线路速率估算发流时长（1400B/帧），用于 .pkt 内 sleep，贴近发完时刻
estimate_send_secs() {
    local count="$1"
    local rate_pct="$2"
    local line_gbps="$3"
    awk -v n="$count" -v pct="$rate_pct" -v gbps="$line_gbps" '
    BEGIN {
        if (n <= 0 || pct <= 0 || gbps <= 0) { print 60; exit }
        frame_bits = 1400 * 8
        wire_bps = gbps * 1e9 * pct / 100.0
        secs = n * frame_bits / wire_bps
        # 留足余量：pcap 循环 + TX 队列 drain，避免 stop/quit 截断尾部
        secs = secs * 2.5 + 8.0
        if (secs < 12) secs = 12
        printf "%d", int(secs + 0.99)
    }'
}

pktgen_send_margin() {
    local count="$1"
    local margin="${PKTGEN_SEND_MARGIN:-}"
    if [[ -z "${margin}" ]]; then
        # 大流量下 pktgen 尾部截断更明显：按 0.5% + 512，上限 16384
        margin=$(( count / 200 + 512 ))
        if [[ "${margin}" -lt 128 ]]; then margin=128; fi
        if [[ "${margin}" -gt 16384 ]]; then margin=16384; fi
    fi
    echo "${margin}"
}

write_cmd_file() {
    local f="$1"
    local wait_secs
    local pktgen_count="${COUNT}"
    local margin=0
    {
        echo "disable screen"
        echo "set 0 rate ${RATE}"
        if [[ -n "${COUNT}" ]]; then
            margin="$(pktgen_send_margin "${COUNT}")"
            pktgen_count=$(( COUNT + margin ))
            wait_secs="$(estimate_send_secs "${pktgen_count}" "${RATE}" "${LINE_GBPS}")"
            echo "set 0 count ${pktgen_count}"
            echo "start 0"
            echo "sleep ${wait_secs}"
            echo "sleep 8"
            echo "enable screen"
            echo "sleep 2"
            echo "page main"
            echo "sleep 2"
        else
            echo "set 0 count 0"
            echo "start 0"
            echo "sleep ${DURATION}"
            echo "sleep 2"
            echo "enable screen"
            echo "sleep 2"
        fi
        echo "quit"
    } > "${f}"
}

CMD_FILE="$(mktemp --suffix=.pkt)"
write_cmd_file "${CMD_FILE}"

echo "---- pktgen 命令 (${CMD_FILE}) ----"
cat "${CMD_FILE}"
if [[ -n "${COUNT}" ]]; then
    margin="$(pktgen_send_margin "${COUNT}")"
    echo "[*] pktgen 目标 ${COUNT} 包 + 余量 ${margin} → set 0 count $((COUNT + margin))"
    echo "[*] 估算发流 $(estimate_send_secs "$((COUNT + margin))" "${RATE}" "${LINE_GBPS}")s 后 quit"
fi

PCAP_ABS="$(cd "$(dirname "${PCAP}")" && pwd)/$(basename "${PCAP}")"

PKTGEN_PID=""
stop_pktgen() {
    if [[ -n "${PKTGEN_PID}" ]] && kill -0 "${PKTGEN_PID}" 2>/dev/null; then
        echo "[*] 停止 pktgen (PID ${PKTGEN_PID}) ..." >&2
        kill -INT "${PKTGEN_PID}" 2>/dev/null || true
        sleep 1
        kill -TERM "${PKTGEN_PID}" 2>/dev/null || true
        wait "${PKTGEN_PID}" 2>/dev/null || true
    fi
}

on_signal() {
    echo "" >&2
    stop_pktgen
    rm -f "${CMD_FILE}" 2>/dev/null || true
    exit 130
}
on_exit() {
    stop_pktgen
    rm -f "${CMD_FILE}" 2>/dev/null || true
}
trap on_signal INT TERM
trap on_exit EXIT

run_pktgen_bg() {
    local tsec="$1"
    shift
    local log_file
    log_file="$(mktemp --suffix=.pktgen.log)"
    echo "[*] 启动 pktgen ...（Ctrl+C 停止，发完自动退出）"
    timeout --preserve-status "${tsec}" "$@" >>"${log_file}" 2>&1 &
    PKTGEN_PID=$!
    wait "${PKTGEN_PID}"
    local rc=$?
    PKTGEN_PID=""
    local tx_pkts=""
    if [[ -f "${log_file}" ]]; then
        cp -f "${log_file}" /tmp/last_pktgen.log 2>/dev/null || true
        tx_pkts="$(grep -Eo 'TX count[[:space:]]*:[[:space:]]*[0-9,]+' "${log_file}" 2>/dev/null \
            | tail -1 | grep -Eo '[0-9,]+' | tr -d ',' || true)"
        if [[ -z "${tx_pkts}" ]]; then
            tx_pkts="$(grep -Eo 'Tx Pkts[[:space:]]*:[[:space:]]*[0-9,]+' "${log_file}" 2>/dev/null \
                | tail -1 | grep -Eo '[0-9,]+' | tr -d ',' || true)"
        fi
    fi
    if [[ "${rc}" -eq 0 ]]; then
        if [[ -n "${tx_pkts}" ]]; then
            echo "[OK]  pktgen 发流完成 (TX=${tx_pkts})"
        else
            echo "[OK]  pktgen 发流完成"
        fi
    elif [[ "${rc}" -eq 124 ]]; then
        echo "[WARN] pktgen 超时 (${tsec}s)，详见 ${log_file}" >&2
    elif [[ "${rc}" -ne 0 ]]; then
        echo "[WARN] pktgen 异常退出 rc=${rc}，详见 ${log_file}" >&2
        tail -20 "${log_file}" >&2 || true
    fi
    if [[ -n "${COUNT}" && -n "${tx_pkts}" ]]; then
        local margin
        margin="$(pktgen_send_margin "${COUNT}")"
        if [[ "${tx_pkts}" -lt "${COUNT}" ]]; then
            echo "[WARN] pktgen TX=${tx_pkts} < 收端期望 ${COUNT}（发端少发）" >&2
        elif [[ "${tx_pkts}" -lt "$((COUNT + margin))" ]]; then
            echo "[WARN] pktgen TX=${tx_pkts} < 发端目标 $((COUNT + margin))（余量未打满，尾部可能截断）" >&2
        fi
    fi
    rm -f "${log_file}" 2>/dev/null || true
    return "${rc}"
}

if [[ -n "${COUNT}" ]]; then
    WAIT_SECS="$(estimate_send_secs "${COUNT}" "${RATE}" "${LINE_GBPS}")"
    run_pktgen_bg $((WAIT_SECS + 120)) \
        "${PKTGEN}" -l "${LCORES}" -n "${CHANNELS}" -a "${PCI}" \
        --file-prefix="${FILE_PREFIX}" -- \
        -P -m "[1:2].0" -s "0:${PCAP_ABS}" -f "${CMD_FILE}" || true
else
    run_pktgen_bg $((DURATION + 120)) \
        "${PKTGEN}" -l "${LCORES}" -n "${CHANNELS}" -a "${PCI}" \
        --file-prefix="${FILE_PREFIX}" -- \
        -P -m "[1:2].0" -s "0:${PCAP_ABS}" -f "${CMD_FILE}" || true
fi
echo "[*] pktgen 结束"
