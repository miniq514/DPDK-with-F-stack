#!/usr/bin/env bash
# scripts/run_recv.sh
#
# 8007 一键 live 收包 + 落盘入口：
#   setup_env.sh（绑 vfio）→ build/fstack_udp_recv → 退出时自动 release_env.sh
#
# 用法:
#   sudo ./scripts/run_recv.sh --link dac --raw-payload \
#       --output test/output/live_dac.bin
#   sudo ./scripts/run_recv.sh --link fibre --raw-payload --payload-size 1344 \
#       --output test/output/live_fibre.bin
#   sudo ./scripts/run_recv.sh --link dac --expected-pkts 10000 --raw-payload \
#       --output test/output/fixed_count.bin
#
#   sudo ./scripts/run_recv.sh --link dac --kni-ops --expected-pkts 0 \
#       --output test/output/kni_idle.bin
#
# 详见 scripts/README.md。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/link_profile.sh
source "${SCRIPT_DIR}/lib/link_profile.sh"

PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FF_PATH="${FF_PATH:-/home/cics/HMH/F-stack}"

LINK="dac"
PROTO="udp"          # udp | tcp
OUTPUT=""
RAW_PAYLOAD=0
DST_PORT=""          # 空 = 由 link_profile 默认（8888）
EXPECTED_PKTS=""
PAYLOAD_SIZE=""
RX_CPU=""
WRITER_CPU=""
NO_RESTORE=0
KNI_OPS=0
EXTRA_APP_ARGS=()
ZC_RECV=0
UDP_BYPASS=0


usage() {
    sed -n '2,18p' "$0"
    echo ""
    echo "Options:"
    echo "  --link dac|fibre         链路（默认 dac）"
    echo "  --proto udp|tcp          协议（默认 udp；tcp 用 fstack_tcp_recv）"
    echo "  --output PATH            落盘文件路径（相对项目根）"
    echo "  --dst-port PORT          bind 端口（默认 dac/fibre 均 8888）"
    echo "  --raw-payload            透传整段 payload / 定长 TCP 帧，不解析 8B 头"
    echo "  --no-raw-payload         解析 8B 头（seq+len）后落 data 区"
    echo "  --payload-size N         raw 模式标称长度（默认 1344）"
    echo "  --expected-pkts N        收满 N 个匹配帧后退出；0=持续至 Ctrl+C"
    echo "  --rx-cpu ID              F-Stack / RX 线程倾向 CPU"
    echo "  --writer-cpu ID          writer pthread 绑核"
    echo "  --kni-ops                启动后给 veth0 配运维 IPv6（…:0004）"
    echo "  --zc-recv                UDP 使用 ff_zc_recv 零拷贝收（需 libfstack 启用）"
    echo "  --udp-bypass             UDP 半旁路：packet_dispatcher 截获 dport，不进 sockbuf"
    echo "  --no-restore             退出时不 release（调试 vfio 状态时用）"
    echo "  -h, --help               显示帮助"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --link)          LINK="$2"; shift 2 ;;
        --proto)         PROTO="$2"; shift 2 ;;
        --output)        OUTPUT="$2"; shift 2 ;;
        --dst-port)      DST_PORT="$2"; shift 2 ;;
        --raw-payload)   RAW_PAYLOAD=1; shift ;;
        --no-raw-payload) RAW_PAYLOAD=0; shift ;;
        --payload-size)  PAYLOAD_SIZE="$2"; shift 2 ;;
        --expected-pkts) EXPECTED_PKTS="$2"; shift 2 ;;
        --rx-cpu)        RX_CPU="$2"; shift 2 ;;
        --writer-cpu)    WRITER_CPU="$2"; shift 2 ;;
        --kni-ops)       KNI_OPS=1; shift ;;
        --zc-recv)       ZC_RECV=1; shift ;;
        --udp-bypass)    UDP_BYPASS=1; shift ;;
        --no-restore)    NO_RESTORE=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        --)              shift; EXTRA_APP_ARGS+=("$@"); break ;;
        *)               echo "未知参数: $1" >&2; usage; exit 1 ;;
    esac
done

case "${PROTO}" in
    udp|tcp) ;;
    *) echo "[FAIL] --proto 须为 udp|tcp，收到: ${PROTO}" >&2; exit 1 ;;
esac

if [[ "${PROTO}" == "tcp" ]]; then
    RECV_BIN="${FSTACK_RECV_BIN:-${PROJECT_ROOT}/build/fstack_tcp_recv}"
    RECV_TARGET="fstack_tcp_recv"
else
    RECV_BIN="${FSTACK_RECV_BIN:-${PROJECT_ROOT}/build/fstack_udp_recv}"
    RECV_TARGET="fstack_udp_recv"
fi

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    echo "[FAIL] 请使用 sudo 运行（需要 vfio 绑定）" >&2
    exit 1
fi

link_profile_apply "${LINK}" || exit 1
link_profile_summary

# 默认端口
[[ -z "${DST_PORT}" ]] && DST_PORT="${LINK_DST_PORT}"
# 默认输出路径
[[ -z "${OUTPUT}" ]] && OUTPUT="test/output/live_${LINK_NAME}.bin"

# 确保输出目录存在
mkdir -p "$(dirname "${PROJECT_ROOT}/${OUTPUT}")"

# 确保二进制已编译
if [[ ! -x "${RECV_BIN}" ]]; then
    echo "[*] 编译 ${RECV_TARGET} ..."
    if command -v cmake &>/dev/null; then
        cmake -S "${PROJECT_ROOT}" -B "${PROJECT_ROOT}/build" \
            -DFF_PATH="${FF_PATH}" >/dev/null
        cmake --build "${PROJECT_ROOT}/build" --target "${RECV_TARGET}" -j"$(nproc)" >/dev/null
    else
        echo "[FAIL] 无 cmake，无法编译 ${RECV_BIN}" >&2
        exit 1
    fi
    [[ -x "${RECV_BIN}" ]] || { echo "[FAIL] 编译失败: ${RECV_BIN}" >&2; exit 1; }
fi

# 退出时自动 release（除非 --no-restore）
RELEASE_RAN=0
cleanup() {
    local rc=$?
    if [[ "${NO_RESTORE}" -eq 1 ]]; then
        echo "[*] --no-restore: 跳过 release_env.sh" >&2
    elif [[ "${RELEASE_RAN}" -eq 0 ]]; then
        echo "" >&2
        echo "[*] 退出收包，自动 release ${LINK_NAME} ..." >&2
        bash "${SCRIPT_DIR}/release_env.sh" --link "${LINK_NAME}" || \
            echo "[WARN] release_env.sh 失败，请手动恢复 ${LINK_IFACE}" >&2
        RELEASE_RAN=1
    fi
    exit "${rc}"
}
trap cleanup EXIT INT TERM

# 1. setup vfio
echo "[*] setup_env: 交给 DPDK（${LINK_IFACE} → ${DPDK_BIND_DRIVER}）"
bash "${SCRIPT_DIR}/setup_env.sh" --link "${LINK_NAME}"

# 2. 组装应用参数（" -- " 之后）
APP_ARGS=()
APP_ARGS+=(--output "${OUTPUT}")
APP_ARGS+=(--dst-port "${DST_PORT}")
if [[ "${RAW_PAYLOAD}" -eq 1 ]]; then
    APP_ARGS+=(--raw-payload)
else
    APP_ARGS+=(--no-raw-payload)
fi
[[ -n "${PAYLOAD_SIZE}" ]]  && APP_ARGS+=(--payload-size "${PAYLOAD_SIZE}")
[[ -n "${EXPECTED_PKTS}" ]] && APP_ARGS+=(--expected-pkts "${EXPECTED_PKTS}")
[[ -n "${RX_CPU}" ]]        && APP_ARGS+=(--rx-cpu "${RX_CPU}")
[[ -n "${WRITER_CPU}" ]]    && APP_ARGS+=(--writer-cpu "${WRITER_CPU}")
[[ "${ZC_RECV}" -eq 1 ]]    && APP_ARGS+=(--zc-recv)
[[ "${UDP_BYPASS}" -eq 1 ]] && APP_ARGS+=(--udp-bypass)
if [[ ${#EXTRA_APP_ARGS[@]} -gt 0 ]]; then
    APP_ARGS+=("${EXTRA_APP_ARGS[@]}")
fi

echo ""
echo "============================================================="
echo "  F-Stack ${PROTO^^} 收包 (8004, ${LINK_NAME})"
echo "============================================================="
echo "  bind ipv6    : ${LINK_IPV6}"
echo "  ops ipv6     : ${LINK_OPS_IPV6} (${LINK_OPS_IFACE}, kni-ops=${KNI_OPS})"
echo "  proto/port   : ${PROTO}/${DST_PORT}"
echo "  output       : ${OUTPUT}"
echo "  raw payload  : ${RAW_PAYLOAD}"
[[ -n "${EXPECTED_PKTS}" ]] && echo "  expected pkts: ${EXPECTED_PKTS}"
echo "  config       : ${FSTACK_CONFIG}"
echo "  binary       : ${RECV_BIN}"
echo "  app args     : ${APP_ARGS[*]}"
echo "============================================================="
echo "[*] 启动收端，Ctrl+C 停止（退出时自动 release ${LINK_IFACE}）"
echo ""

# KNI：F-Stack 热插拔 virtio_user 后才会出现 veth0；后台短轮询配置运维 IP
if [[ "${KNI_OPS}" -eq 1 ]]; then
    echo "[*] --kni-ops: 后台等待 ${LINK_OPS_IFACE} 并配置 ${LINK_OPS_IPV6}"
    (
        bash "${SCRIPT_DIR}/setup_kni_ops.sh" --link "${LINK_NAME}" --wait-sec 10
    ) >"${PROJECT_ROOT}/test/output/kni_ops_setup.log" 2>&1 &
    KNI_OPS_PID=$!
    echo "[*] kni-ops helper pid=${KNI_OPS_PID}, log=test/output/kni_ops_setup.log"
fi

# 3. 运行收端（KNI 时 --iova-mode=va 由 F-Stack ff_config 在 kni.enable 时注入 EAL）
"${RECV_BIN}" -c "${FSTACK_CONFIG}" -- "${APP_ARGS[@]}"
