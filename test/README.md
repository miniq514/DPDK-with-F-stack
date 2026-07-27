# test/ — 落盘输出与校验

本目录存放 **live 打流落盘文件**、**运行日志** 及 **输出校验脚本**。路径均相对 `Fstack-DPDk` 项目根，便于与 `scripts/run_recv.sh --output` 配合。

> **状态：** v1.0 实现中；目录与脚本按设计约定创建，细节可后续调整。

---

## 目录说明

| 路径 | 用途 | 是否入库 |
|------|------|----------|
| `test/output/` | 运行时 `.bin` 落盘文件 | 否（`.gitignore`） |
| `test/logs/` | `run_recv.sh` / 收端 stderr 日志副本 | 否 |
| `test/verify_output.sh` | 落盘文件大小与内容校验 | 是 |
| `test/self_loop_pcap.sh` | pcap 自环测试（程序A 生成 → 收端解析逻辑回读校验） | 是 |
| `test/self_loop_pcap_check.py` | 自环测试的 pcap 校验器（被 `self_loop_pcap.sh` 调用） | 是 |

**默认输出文件示例：**

```
test/output/live.bin           # run_recv.sh 默认
test/output/live_dac.bin       # DAC 联调
test/output/live_fibre.bin     # fibre / 交换板 raw
```

---

## `verify_output.sh`

**用途：** 校验落盘文件是否存在、大小是否符合预期，并对 **非 raw / 递增字节测试流** 做内容 pattern 检查（逻辑继承自 `receiver_dpdk/Test/verify_udp_output.sh`）。

**用法：**

```bash
cd /home/dev/HMH--8007/Fstack-DPDk

# 仅检查文件存在 + 打印大小与 SHA256
bash test/verify_output.sh test/output/live_dac.bin

# 检查大小必须等于 expected_bytes
bash test/verify_output.sh test/output/live_dac.bin 1344000

# 配合 run_recv：收满 N 包、每包 payload 1344B（raw 透传）
# expected_bytes = N * 1344
bash test/verify_output.sh test/output/fixed_count.bin $((10000 * 1344))
```

**参数：**

| 位置 | 名称 | 必填 | 说明 |
|------|------|------|------|
| `$1` | `output_file` | 是 | 落盘文件路径 |
| `$2` | `expected_bytes` | 否 | 预期文件字节数；`0` 或未传则 **跳过** 大小与内容 pattern 检查 |

**检查项：**

1. 文件存在
2. `stat` 实际大小（并打印 MB）
3. `sha256sum`
4. 若 `expected_bytes > 0`：大小必须相等；并按 **递增字节** `data[i] == i & 0xFF` 校验（适用于 gen_udp_pcap / 标准测试流）

**raw 模式说明：**

- **交换板真实 1344B 流**：通常只做 **大小 / SHA256** 对比，或按包数 × 标称长度算 `expected_bytes`；内容未必是递增 pattern，此时传 `expected_bytes` 做大小校验即可，**不要**依赖递增字节检查（脚本对 raw 相机流可能需 `--skip-pattern`，v1.0 若未实现则只传 `expected_bytes=0` 做存在性检查）。
- **8B 头测试流**（`--no-raw-payload`）：落盘为纯 data 区，可用递增 pattern 校验。

**退出码：**

| 码 | 含义 |
|----|------|
| `0` | 全部检查通过 |
| `1` | 用法错误或检查失败 |

---

## `self_loop_pcap.sh`（pcap 自环测试）

**用途：** 不经真实网卡，验证 sender 程序A `gen_udp_pcap` 生成的 IPv6 pcap 与收端 `include/udp_payload.h` 解析逻辑完全兼容。自动编译 `gen_udp_pcap`、生成 pcap、回读校验。

**校验项：**
- pcap 文件头 magic / linktype=1
- 每包 EtherType `0x86dd`、IPv6 version=6 nexthdr=17(UDP) hlim=64
- UDP dst_port=8888、UDP length 自洽
- UDP checksum 按 IPv6 pseudo header 重算并比对（IPv6 必须非零）
- payload 8B 头：`seq` 递增 0,1,2,...；`declared_len == payload_size`
- data 区递增字节 `(i*payload_size + j) & 0xFF`（跨包连续，与 `verify_output.sh` 一致）
- 包数 == count

**用法：**

```bash
cd /home/dev/HMH--8007/Fstack-DPDk

# 默认 count=1000 payload=1344，测 --to-8007 与 --to-8004 两个目标
bash test/self_loop_pcap.sh

# 自定义
bash test/self_loop_pcap.sh --count 5000 --payload-size 1344
bash test/self_loop_pcap.sh --target 8007        # 仅测一个目标
```

**参数：**

| 参数 | 默认 | 说明 |
|------|------|------|
| `--count N` | `1000` | 生成包数 |
| `--payload-size B` | `1344` | payload 数据区字节（奇数会触发 checksum 补零路径） |
| `--target 8007\|8004` | `8007 8004` | 测试目标（可只传一个） |

**退出码：** `0`=全部通过，非 0=存在失败项。

---

## 与 `run_recv.sh` 的配合

**持续打流（不验大小）：**

```bash
sudo ./scripts/run_recv.sh --link dac --raw-payload \
    --output test/output/live_dac.bin
# 停止后：
bash test/verify_output.sh test/output/live_dac.bin
```

**收满固定包数后自动退出 + 校验：**

```bash
PKTS=10000
PAYLOAD=1344
sudo ./scripts/run_recv.sh --link dac --raw-payload \
    --expected-pkts "$PKTS" --payload-size "$PAYLOAD" \
    --output test/output/fixed_count.bin

bash test/verify_output.sh test/output/fixed_count.bin $((PKTS * PAYLOAD))
```

**8B 头模式（非 raw）：**

```bash
# payload 数据区默认 1400B，expected = pkts * 1400
bash test/verify_output.sh test/output/proto.bin $((100 * 1400))
```

---

## `test/logs/`

**用途：** 保存 live 联调日志（可选，由 `run_recv.sh` 重定向或 tee）。

**命名建议：**

```
test/logs/recv_dac_20260629_143022.log
test/logs/recv_fibre_20260629_150000.log
```

**可变参数（计划由 `run_recv.sh` 支持）：**

| 参数 | 默认 | 说明 |
|------|------|------|
| `--log-dir` | `test/logs` | 日志目录 |
| `--log-file PATH` | 自动生成时间戳 | 指定日志文件 |

---

## 落盘口径（与 receiver_dpdk 一致）

| 项目 | 说明 |
|------|------|
| 写路径 API | `udp_writer_submit(buf, len)` |
| 聚合 | 256KB staging |
| O_DIRECT | 默认开启 |
| raw | 整段 UDP payload 写入 |
| 非 raw | 剥离 8B 头后写 data 区 |

校验脚本只关心 **最终 `.bin` 文件**，与收包前端是 F-Stack 还是 DPDK 无关。

---

## 后续可能扩展（未实现）

| 脚本 | 说明 |
|------|------|
| `test/check_nic.sh` | 收端启动前检查 SSH 口未绑 vfio、链路 up |
| `test/compare_runs.sh` | 两次落盘 SHA256 / 大小对比 |
| pcap 离线 E2E | v1.0 不做；后续若做，输出仍进 `test/output/` |
