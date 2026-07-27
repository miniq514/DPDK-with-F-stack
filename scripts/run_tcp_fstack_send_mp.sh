#!/usr/bin/env bash
# scripts/run_tcp_fstack_send_mp.sh
#
# F-Stack 多进程 TCP 发端：每核一条连接，帧长仍为 8+1336=1344B。
# 用法（8007）:
#   sudo FF_PATH=/home/dev/HMH--8007/F-stack \
#     ./scripts/run_tcp_fstack_send_mp.sh --to-8004 --workers 4 --count 1000000

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/link_profile.sh
source "${SCRIPT_DIR}/lib/link_profile.sh"

PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FF_PATH="${FF_PATH:-/home/dev/HMH--8007/F-stack}"
SEND_BIN="${PROJECT_ROOT}/build/fstack_tcp_send"
CFG_TMPL="${PROJECT_ROOT}/configs/config.send.dac.mp.ini"
CFG_RUN="${PROJECT_ROOT}/configs/.config.send.dac.mp.run.ini"
LOG_DIR="${PROJECT_ROOT}/test/output"

TO_FLAG="--to-8004"
COUNT=100000
WORKERS=4
RATE_MBPS=0
PAYLOAD_SIZE=1336
DST_PORT=8888
RAW=1
NO_RESTORE=0
LINK=dac

usage() {
    cat <<EOF
Usage: sudo FF_PATH=... $0 [options]
  --workers N            F-Stack 进程数 / 连接数（默认 4）
  --count N              总帧数（均分到各 worker）
  --rate-mbps M          总目标速率；0=尽力；分到每 worker = M/N
  --to-8004 / --to-8007
  --payload-size B       body（帧=8+B，默认 1336 → 1344）
  --no-restore
EOF
}

lcore_mask_hex() {
    local n=$1
    local mask=0
    local i
    for ((i = 0; i < n; i++)); do
        mask=$((mask | (1 << i)))
    done
    printf '%x' "${mask}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --to-8004) TO_FLAG="--to-8004"; shift ;;
        --to-8007) TO_FLAG="--to-8007"; shift ;;
        --count) COUNT="$2"; shift 2 ;;
        --workers) WORKERS="$2"; shift 2 ;;
        --rate-mbps) RATE_MBPS="$2"; shift 2 ;;
        --payload-size) PAYLOAD_SIZE="$2"; shift 2 ;;
        --dst-port) DST_PORT="$2"; shift 2 ;;
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
if [[ "${WORKERS}" -lt 1 || "${WORKERS}" -gt 16 ]]; then
    echo "[FAIL] --workers 须在 1..16" >&2
    exit 1
fi

link_profile_apply "${LINK}" || exit 1

if [[ ! -x "${SEND_BIN}" ]]; then
    echo "[*] 编译 fstack_tcp_send ..."
    cmake -S "${PROJECT_ROOT}" -B "${PROJECT_ROOT}/build" -DFF_PATH="${FF_PATH}" >/dev/null
    cmake --build "${PROJECT_ROOT}/build" --target fstack_tcp_send -j"$(nproc)"
fi

mkdir -p "${LOG_DIR}"
rm -f "${LOG_DIR}"/tcp_mp_w*.log
MASK="$(lcore_mask_hex "${WORKERS}")"
sed "s/^lcore_mask=.*/lcore_mask=${MASK}/" "${CFG_TMPL}" > "${CFG_RUN}"
echo "[*] mp config lcore_mask=${MASK} workers=${WORKERS}"

RELEASE_RAN=0
PIDS=()
cleanup() {
    local rc=$?
    local p
    for p in "${PIDS[@]:-}"; do
        kill -TERM "${p}" 2>/dev/null || true
    done
    sleep 0.5
    for p in "${PIDS[@]:-}"; do
        kill -KILL "${p}" 2>/dev/null || true
    done
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

# primary 只保活：TCP 打流全由 secondary 承担（规避 primary 上 SYN-ACK RSS 丢包）
nsenders=${WORKERS}
if [[ "${WORKERS}" -gt 1 ]]; then
    nsenders=$((WORKERS - 1))
fi
base=$((COUNT / nsenders))
rem=$((COUNT % nsenders))
per_rate=0
if [[ "${RATE_MBPS}" != "0" ]]; then
    per_rate=$((RATE_MBPS / nsenders))
    if [[ "${per_rate}" -lt 1 ]]; then
        per_rate=1
    fi
fi

echo "============================================================="
echo "  F-Stack TCP 多进程发端 (${LINK_NAME})"
echo "  workers=${WORKERS} (primary idle + ${nsenders} senders when workers>1)"
echo "  total_count=${COUNT} rate_total=${RATE_MBPS}"
echo "============================================================="

for ((w = 0; w < WORKERS; w++)); do
    wcount=0
    if [[ "${w}" -eq 0 && "${WORKERS}" -gt 1 ]]; then
        APP=("${TO_FLAG}" --worker-id 0 --workers "${WORKERS}"
             --idle-primary --linger-sec 120 --count 1)
    else
        if [[ "${WORKERS}" -eq 1 ]]; then
            wcount=${COUNT}
        else
            sid=$((w - 1))
            wcount=${base}
            if [[ "${sid}" -eq $((nsenders - 1)) ]]; then
                wcount=$((base + rem))
            fi
        fi
        APP=(--count "${wcount}" --dst-port "${DST_PORT}" --payload-size "${PAYLOAD_SIZE}"
             "${TO_FLAG}" --worker-id "${w}" --workers "${WORKERS}")
        if [[ "${per_rate}" != "0" ]]; then
            APP+=(--rate-mbps "${per_rate}")
        fi
        if [[ "${RAW}" -eq 1 ]]; then
            APP+=(--raw-payload)
        else
            APP+=(--no-raw-payload)
        fi
    fi

    wlog="${LOG_DIR}/tcp_mp_w${w}.log"
    if [[ "${w}" -eq 0 ]]; then
        ptype=primary
    else
        ptype=secondary
    fi

    echo "[*] start worker ${w}/${WORKERS} proc-type=${ptype} count=${wcount}"
    "${SEND_BIN}" -c "${CFG_RUN}" --proc-type="${ptype}" --proc-id="${w}" -- \
        "${APP[@]}" > "${wlog}" 2>&1 &
    PIDS+=($!)

    if [[ "${w}" -eq 0 ]]; then
        for i in $(seq 1 80); do
            if grep -qE 'idle-primary|F-Stack TCP TX:|Successed to register|Addr6:' "${wlog}" 2>/dev/null; then
                break
            fi
            if ! kill -0 "${PIDS[0]}" 2>/dev/null; then
                echo "[FAIL] primary 提前退出，见 ${wlog}" >&2
                tail -40 "${wlog}" >&2 || true
                exit 1
            fi
            sleep 0.25
        done
        sleep 1.0
    else
        sleep 0.15
    fi
done

echo "[*] 全部 worker 已拉起"

# 等每个 worker 打出统计或明确失败（primary linger 前会先打印统计）
for ((w = 0; w < WORKERS; w++)); do
    wlog="${LOG_DIR}/tcp_mp_w${w}.log"
    for i in $(seq 1 240); do
        if grep -qE 'F-Stack TCP TX Statistics|connect timeout|connect SO_ERROR|ff_init failed' "${wlog}" 2>/dev/null; then
            break
        fi
        sleep 0.25
    done
done

rc=0
for idx in "${!PIDS[@]}"; do
    p=${PIDS[$idx]}
    if [[ "${idx}" -eq 0 ]]; then
        continue
    fi
    if ! wait "${p}"; then
        rc=1
    fi
done
if [[ -n "${PIDS[0]:-}" ]]; then
    kill -TERM "${PIDS[0]}" 2>/dev/null || true
    wait "${PIDS[0]}" 2>/dev/null || true
fi

echo
echo "========== MP aggregate (sum of workers) =========="
awk '
  /tx_frames/          { f+=$3 }
  /tx_bytes/           { b+=$3 }
  /achieved_mbps/      { mb+=$3; n++ }
  /write_block_ms/     { wb+=$3 }
  END {
    printf "  workers_ok_lines   : %d\n", n
    printf "  tx_frames_sum      : %d\n", f
    printf "  tx_bytes_sum       : %d\n", b
    printf "  achieved_mbps_sum  : %.1f  (sum of per-worker rates)\n", mb
    printf "  write_block_ms_sum : %.3f\n", wb
  }
' "${LOG_DIR}"/tcp_mp_w*.log
echo "  per-worker logs: ${LOG_DIR}/tcp_mp_w*.log"
echo "==================================================="

exit "${rc}"
