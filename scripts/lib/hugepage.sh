#!/usr/bin/env bash
# scripts/lib/hugepage.sh
#
# 继承本机已配置的 hugetlbfs（2MB 大页），仅在不足时尝试扩容。
# 供 setup_env.sh / run_recv.sh / sender 共用。

# 收端 F-Stack + mempool 最低需求（页数 × 2MB）；本机若已配好 2048 页则直接复用。
HUGEPAGE_MIN_TOTAL="${HUGEPAGE_MIN_TOTAL:-512}"
HUGEPAGE_MIN_FREE="${HUGEPAGE_MIN_FREE:-256}"
HUGEPAGE_EXPAND_TARGET="${HUGEPAGE_EXPAND_TARGET:-4G}"
HUGEPAGE_FILE_PREFIX="${HUGEPAGE_FILE_PREFIX:-fstack_recv8004}"

hugepages_2m_total() {
    local n
    n="$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)"
    if [[ "${n}" =~ ^[0-9]+$ ]] && [[ "${n}" -gt 0 ]]; then
        echo "${n}"
        return 0
    fi
    # NUMA 节点汇总
    local sum=0 f v
    for f in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages; do
        [[ -f "${f}" ]] || continue
        v="$(cat "${f}" 2>/dev/null || echo 0)"
        [[ "${v}" =~ ^[0-9]+$ ]] || v=0
        sum=$((sum + v))
    done
    echo "${sum}"
}

hugepages_2m_free() {
    awk '/^HugePages_Free:/{print $2; exit}' /proc/meminfo 2>/dev/null || echo 0
}

hugepages_2m_used() {
    local total free
    total="$(hugepages_2m_total)"
    free="$(hugepages_2m_free)"
    echo $((total - free))
}

hugepage_summary() {
    local total free used
    total="$(hugepages_2m_total)"
    free="$(hugepages_2m_free)"
    used="$(hugepages_2m_used)"
    echo "  hugepages 2MB : total=${total} free=${free} used=${used} (hugetlbfs=/dev/hugepages)"
}

hugepages_inherit_ready() {
    local total free
    total="$(hugepages_2m_total)"
    free="$(hugepages_2m_free)"
    [[ "${total}" -ge "${HUGEPAGE_MIN_TOTAL}" && "${free}" -ge "${HUGEPAGE_MIN_FREE}" ]]
}

# 仅清理本项目的 stale map（不 wipe 整池，避免影响其它 DPDK 进程）
hugepages_cleanup_project_maps() {
    local prefix="${1:-${HUGEPAGE_FILE_PREFIX}}"
    local n=0
    shopt -s nullglob
    for f in /dev/hugepages/"${prefix}"*; do
        rm -f "${f}" 2>/dev/null || true
        n=$((n + 1))
    done
    shopt -u nullglob
    if [[ "${n}" -gt 0 ]]; then
        echo "[*] 已清理本项目 stale hugetlb 映射: ${prefix}* (${n} 个文件)"
    fi
}

# 继承本机 hugepage：够则直接通过；不够才尝试 dpdk-hugepages.py 扩容
hugepages_ensure_inherit() {
    local total free
    total="$(hugepages_2m_total)"
    free="$(hugepages_2m_free)"

    echo "[*] 检查本机 hugepages（继承模式，不强制重配）"
    hugepage_summary

    if hugepages_inherit_ready; then
        echo "[OK]  本机 hugepages 已满足收端需求 (total>=${HUGEPAGE_MIN_TOTAL}, free>=${HUGEPAGE_MIN_FREE})"
        return 0
    fi

    echo "[WARN] free=${free} 或 total=${total} 偏低，尝试扩容到 ${HUGEPAGE_EXPAND_TARGET} ..."

    if ! command -v dpdk-hugepages.py &>/dev/null; then
        echo "[FAIL] 未找到 dpdk-hugepages.py，且本机 free=${free} < ${HUGEPAGE_MIN_FREE}" >&2
        echo "       请在本机预先配置，例如:" >&2
        echo "         echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages" >&2
        return 1
    fi

    # 仅在本项目前缀下清理，避免误删其它任务占用
    hugepages_cleanup_project_maps "${HUGEPAGE_FILE_PREFIX}"

    if ! dpdk-hugepages.py -p 2M --setup "${HUGEPAGE_EXPAND_TARGET}" 2>&1; then
        total="$(hugepages_2m_total)"
        free="$(hugepages_2m_free)"
        if [[ "${total}" -ge "${HUGEPAGE_MIN_TOTAL}" && "${free}" -ge "${HUGEPAGE_MIN_FREE}" ]]; then
            echo "[OK]  扩容报错但当前 free=${free} 已够用，继续"
            return 0
        fi
        echo "[FAIL] hugepages 不足且扩容失败 (total=${total} free=${free})" >&2
        echo "       若 /dev/hugepages 被其它进程占满，请先退出 pktgen/F-Stack，或手动:" >&2
        echo "         sudo rm -f /dev/hugepages/udp_pktgen_txmap_*" >&2
        return 1
    fi

    echo "[OK]  hugepages 扩容完成"
    hugepage_summary
}
