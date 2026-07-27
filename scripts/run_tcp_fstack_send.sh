#!/usr/bin/env bash
# scripts/run_tcp_fstack_send.sh
#
# F-Stack TCP 发端一键打流：setup_env →（后台注入静态 ND）→ fstack_tcp_send → release
#
# 用法（在 8007 上）:
#   sudo FF_PATH=/home/dev/HMH--8007/F-stack ./scripts/run_tcp_fstack_send.sh \
#       --to-8004 --count 100000 --rate-mbps 0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/link_profile.sh
source "${SCRIPT_DIR}/lib/link_profile.sh"

PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FF_PATH="${FF_PATH:-/home/dev/HMH--8007/F-stack}"
SEND_BIN="${PROJECT_ROOT}/build/fstack_tcp_send"
SEND_CONFIG="${PROJECT_ROOT}/configs/config.send.dac.ini"
NDP_BIN="${FF_PATH}/tools/sbin/ndp"

TO_FLAG="--to-8004"
COUNT=100
RATE_MBPS=0
PAYLOAD_SIZE=1336
DST_PORT=8888
RAW=1
NO_RESTORE=0
LINK=dac
DST_IP="1111:2222:1111:2222:1111:2222:1111:0003"
DST_MAC="90:e2:ba:50:7c:f9"

usage() {
    cat <<EOF
Usage: sudo FF_PATH=... $0 [options]
  --to-8004 / --to-8007
  --count N
  --rate-mbps M          0=尽力发
  --payload-size B
  --dst-port P
  --dst-mac MAC
  --raw-payload / --no-raw-payload
  --no-restore
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --to-8004)
            TO_FLAG="--to-8004"
            DST_IP="1111:2222:1111:2222:1111:2222:1111:0003"
            DST_MAC="90:e2:ba:50:7c:f9"
            shift ;;
        --to-8007)
            TO_FLAG="--to-8007"
            DST_IP="1111:2222:1111:2222:1111:2222:1111:0002"
            DST_MAC="90:e2:ba:50:aa:71"
            shift ;;
        --count) COUNT="$2"; shift 2 ;;
        --rate-mbps) RATE_MBPS="$2"; shift 2 ;;
        --payload-size) PAYLOAD_SIZE="$2"; shift 2 ;;
        --dst-port) DST_PORT="$2"; shift 2 ;;
        --dst-mac) DST_MAC="$2"; shift 2 ;;
        --raw-payload) RAW=1; shift ;;
        --no-raw-payload) RAW=0; shift ;;
        --no-restore) NO_RESTORE=1; shift ;;
        --link) LINK="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1" >&2; usage; exit 1 ;;
    esac
done

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    echo "[FAIL] 请 sudo 运行" >&2
    exit 1
fi

link_profile_apply "${LINK}" || exit 1

if [[ ! -x "${SEND_BIN}" ]]; then
    echo "[*] 编译 fstack_tcp_send ..."
    cmake -S "${PROJECT_ROOT}" -B "${PROJECT_ROOT}/build" -DFF_PATH="${FF_PATH}" >/dev/null
    cmake --build "${PROJECT_ROOT}/build" --target fstack_tcp_send -j"$(nproc)"
fi

RELEASE_RAN=0
cleanup() {
    local rc=$?
    if [[ "${NO_RESTORE}" -eq 0 && "${RELEASE_RAN}" -eq 0 ]]; then
        echo "[*] 发端退出，release ${LINK_NAME} ..." >&2
        bash "${SCRIPT_DIR}/release_env.sh" --link "${LINK_NAME}" || true
        RELEASE_RAN=1
    fi
    exit "${rc}"
}
trap cleanup EXIT INT TERM

echo "[*] setup_env ${LINK_NAME}"
bash "${SCRIPT_DIR}/setup_env.sh" --link "${LINK_NAME}"

APP=(--count "${COUNT}" --dst-port "${DST_PORT}" --payload-size "${PAYLOAD_SIZE}" "${TO_FLAG}")
if [[ "${RATE_MBPS}" != "0" ]]; then
    APP+=(--rate-mbps "${RATE_MBPS}")
fi
if [[ "${RAW}" -eq 1 ]]; then
    APP+=(--raw-payload)
else
    APP+=(--no-raw-payload)
fi

echo "============================================================="
echo "  F-Stack TCP 发端 (${LINK_NAME})"
echo "  config : ${SEND_CONFIG}"
echo "  dst    : [${DST_IP}] mac=${DST_MAC}"
echo "  args   : ${APP[*]}"
echo "============================================================="

# 动态 ND 在 kni=0 时可用；tools/ndp IPC 在本环境会 SIGSEGV，故不强制注入
echo "[*] 依赖 F-Stack 动态 ND（kni 关闭时）" >&2

set +e
"${SEND_BIN}" -c "${SEND_CONFIG}" -- "${APP[@]}"
rc=$?
set -e

exit "${rc}"
