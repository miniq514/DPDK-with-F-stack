#!/usr/bin/env bash
# test/self_loop_pcap.sh
#
# pcap 自环测试：用程序A gen_udp_pcap 生成 IPv6 pcap，再用收端 udp_payload.h
# 的解析逻辑回读校验，验证 sender 输出与收端解析完全兼容。
#
# 自环语义：发端生成 → 喂回收端校验逻辑（不经真实网卡），确认：
#   - EtherType 0x86dd / IPv6 nexthdr=17 / UDP dst_port=8888
#   - UDP checksum 按 IPv6 pseudo header 正确且非零
#   - payload 8B 头 seq 递增、declared_len == payload_size
#   - data 区递增字节 (i*payload_size + j) & 0xFF（与 verify_output.sh 一致）
#
# 用法:
#   bash test/self_loop_pcap.sh                 # 默认 count=1000 payload=1336，测 --to-8007 与 --to-8004
#   bash test/self_loop_pcap.sh --count 5000 --payload-size 1336
#   bash test/self_loop_pcap.sh --target 8007   # 仅测一个目标

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
GEN="${ROOT}/build/sender/gen_udp_pcap"

COUNT=1000
PAYLOAD=1336
TARGETS="8007 8004"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --count)        COUNT="$2"; shift 2 ;;
        --payload-size) PAYLOAD="$2"; shift 2 ;;
        --target)       TARGETS="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 1 ;;
    esac
done

if [[ ! -x "${GEN}" ]]; then
    echo "[*] 编译 gen_udp_pcap ..."
    cmake -S "${ROOT}" -B "${ROOT}/build" >/dev/null
    cmake --build "${ROOT}/build" --target gen_udp_pcap -j"$(nproc)" >/dev/null
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

OVERALL_RC=0
for tgt in ${TARGETS}; do
    echo ""
    echo "============================================================"
    echo "  自环测试: --to-${tgt}  count=${COUNT}  payload=${PAYLOAD}"
    echo "============================================================"
    PCAP="${TMP}/loop_${tgt}.pcap"
    "${GEN}" "${PCAP}" "--to-${tgt}" --count "${COUNT}" --payload-size "${PAYLOAD}" >/dev/null

    python3 "${SCRIPT_DIR}/self_loop_pcap_check.py" "${PCAP}" "${COUNT}" "${PAYLOAD}" "${tgt}" || OVERALL_RC=$?
done

echo ""
if [[ "${OVERALL_RC}" -eq 0 ]]; then
    echo "[PASS] pcap 自环测试全部通过 (targets: ${TARGETS})"
else
    echo "[FAIL] pcap 自环测试存在失败项 (rc=${OVERALL_RC})" >&2
fi
exit "${OVERALL_RC}"
