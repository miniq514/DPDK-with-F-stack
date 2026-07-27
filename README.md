# Fstack-DPDk

> **文档版本：** 2026-07-27（对照本仓库当前代码）  
> **固定地址：** 8007 恒 `…:0002`，8004 恒 `…:0003`（不随收/发角色变）  
> **当前默认拓扑：** 8007 发 → 8004 收（dac）

F-Stack **UDP/TCP** 收包 + io_uring 落盘。发端：UDP 用 pktgen；TCP 可用内核 `tcp_stream_send` 或 F-Stack `fstack_tcp_send`。全程 IPv6。支持 **dac** / **fibre**；两机角色可互换。**KNI 运维**用 `veth0` 上 `…:0004`。

**启动命令 → [command.md](command.md)** · **版本与缺口 → [docs/迭代计划.md](docs/迭代计划.md)** · **性能 → [docs/性能统计.md](docs/性能统计.md)**

---

## 能力一览

| 能力 | 状态 | 典型数字 / 入口 |
|------|------|-----------------|
| UDP 收 + 落盘（栈） | ✅ | ~6 Gbps · `run_recv.sh --proto udp` |
| UDP 半旁路 | ✅ | 不限速 ~9.4G；9G 目标 ~8.5G · `--udp-bypass` |
| TCP 收 + 落盘 | ✅ | `run_recv.sh --proto tcp` |
| UDP 发（pktgen） | ✅ | 可打 ~10G 线速 · `sender/run_pktgen_sender.sh` |
| TCP 发（内核） | ✅ | ~2.2 Gbps · `sender/run_tcp_sender.sh` |
| TCP 发（F-Stack 单流） | ✅ | ~2.95–3.6 Gbps · `run_tcp_fstack_send.sh` |
| TCP 多核发（F-Stack MP） | ⚠ 实验 | 握手不稳 · `run_tcp_fstack_send_mp.sh` |
| KNI 运维 ping/SSH | ✅ | `run_recv.sh --kni-ops` · `…:0004` |

---

## 角色互换（4 组合）

| 本端 | 本端角色 | 本端程序 | 关键参数 | 对端 |
|------|----------|----------|----------|------|
| **8004** | 收端（默认） | `scripts/run_recv.sh` | `--link dac`，bind `…:0003` | 8007 sender：`--to-8004` |
| **8004** | 发端 | `sender/run_pktgen_sender.sh` | `--to-8007` | 8007 收端：`--link dac` |
| **8007** | 发端（默认） | `sender/run_pktgen_sender.sh` | `--to-8004` | 8004 收端 |
| **8007** | 收端 | `scripts/run_recv.sh` | `--link dac`，bind `…:0002` | 8004 sender：`--to-8007` |

详细命令见 [command.md](command.md) 场景 A/B。fibre **仅 8007 f0 作收端**（交换板发流）；角色互换只在 dac。

---

## KNI 联调（V3.0）

| 项 | 值 |
|----|-----|
| 业务（F-Stack） | 本机业务 IPv6 + UDP/TCP **8888**（`config.dac.ini` 中 `method=reject`） |
| 运维（KNI → `veth0`） | `…:0004/64` |
| 对端 | 对端内核 f1 业务地址（勿绑 DPDK） |

```bash
# 收端本机（示例：8004）
sudo FF_PATH=/home/cics/HMH/F-stack ./scripts/run_recv.sh \
  --link dac --kni-ops --expected-pkts 0 --output test/output/kni_idle.bin

# 对端
ping6 -c 3 -W 1 1111:2222:1111:2222:1111:2222:1111:0004
ssh -6 -o ConnectTimeout=5 cics@1111:2222:1111:2222:1111:2222:1111:0004 'echo kni-ok; ls'
```

验收（2026-07-13）：ping6 3/3、SSH OK。重编带 KNI 的 `libfstack` 时须优先 `$FF_PATH/dpdk-install/include`。

---

## 项目定位

从原生 DPDK 收包迁到 F-Stack 用户态栈，**落盘层（io_uring）已迁入本仓**；UDP 半旁路（`--udp-bypass`）亦在本仓实现，**不依赖** sibling `UDP_DPDK`。

| 路径 | 说明 | 实测上限（82599） |
|------|------|-------------------|
| 栈收包 | `ff_recvfrom` → ring → writer | ~6 Gbps |
| 半旁路 | `packet_dispatcher` 截获 dport，不进 sockbuf | 线速 ~9.4G |
| 对照（可选） | 早期原生 DPDK 工程同硬件基线 | ~9.8 Gbps |

---

## 拓扑与地址

| 角色 | 节点 | 接口 | IPv6 | MAC（f1） | 端口 |
|------|------|------|------|-----------|------|
| dac 收（默认） | **8004** | f1 | `…:0003` | `90:e2:ba:50:7c:f9` | 8888 |
| dac 发（默认） | **8007** | f1 | `…:0002` | `90:e2:ba:50:aa:71` | →8888 |
| KNI 运维 | 收端本机 | veth0 | `…:0004` | — | 22 等 |
| fibre 收 | **8007** | f0 | `…:0002` | `90:e2:ba:50:aa:70` | 8888 |
| fibre 发 | 交换板 | — | `…:0001` | — | 8888 |

- 发端用 `--to-8004` / `--to-8007`（自动选 dst IP/MAC）；可用 `--src-ip` / `--dst-mac` 覆盖
- 收端 bind：`demo_udp_config.h` 的 `UDP_RX_BIND_IP6` + `configs/config.*.ini` 的 `addr6`（换机作收端须对齐并重编）；`run_recv.sh` 会传 `--dst-port`（默认 **8888**）
- 联调请显式加 `--raw-payload`（脚本/二进制默认均为解析 8B 头，非 raw）

---

## 工程路径

| 机器 | 工程 | F-Stack | 默认角色 |
|------|------|---------|----------|
| 8004 | `/home/cics/HMH/Fstack-DPDk_8004` | `/home/cics/HMH/F-stack` | dac 收端 |
| 8007 | `/home/dev/HMH--8007/Fstack-DPDk` | `/home/dev/HMH--8007/F-stack` | dac 发端 |

两端文档保持同步；本机特有驱动差异见 [scripts/README.md](scripts/README.md)。

---

## 目录结构

```
Fstack-DPDk_8004/
├── README.md / command.md
├── src/                 # fstack_udp_recv / fstack_tcp_recv / fstack_tcp_send
├── sender/              # gen_udp_pcap、pktgen、tcp_stream_send
├── configs/             # dac / fibre / tcpbench / send*
├── scripts/             # setup / release / run_recv / kni / tcp_fstack_send*
├── io-uring/            # udp_writer
├── include/             # demo_udp_config.h 等
├── test/                # 落盘校验、internet 配网
└── docs/                # 迭代计划、性能统计、错误日志
```

收端数据流：`ff_run` /（半旁路）`packet_dispatcher` → `rte_ring` → writer → `io_uring` 落盘。

---

## 构建

```bash
# 8004（推荐显式指定 FF_PATH）
cmake -S . -B build -DFF_PATH=/home/cics/HMH/F-stack
# 8007
# cmake -S . -B build -DFF_PATH=/home/dev/HMH--8007/F-stack
cmake --build build -j"$(nproc)"
```

产物：`fstack_udp_recv`、`fstack_tcp_recv`、`fstack_tcp_send`、`sender/gen_udp_pcap`、`sender/tcp_stream_send`、`libudp_writer.a`

---

## 文档索引

| 文档 | 内容 |
|------|------|
| [command.md](command.md) | 启动命令（场景 A–E、发端参数） |
| [docs/迭代计划.md](docs/迭代计划.md) | 里程碑、缺口、下一步 |
| [docs/性能统计.md](docs/性能统计.md) | 8004/8007 收端实测（统一） |
| [docs/错误日志及改进.md](docs/错误日志及改进.md) | 联调问题与修复 |
| [docs/README.md](docs/README.md) | 设计约束与数据流 |
| [scripts/README.md](scripts/README.md) | setup/release、两机驱动差异 |
| [test/README.md](test/README.md) | 落盘校验与自环测试 |
| [test/internet/README.md](test/internet/README.md) | 内核态 IPv6 配网 / ping6 |

---

## 依赖

| 路径 | 用途 |
|------|------|
| `F-stack/` | **必需**：`libfstack.a`、DPDK |

`UDP_DPDK` 非依赖，仅作历史性能对照（同硬件 ~9.8G）；半旁路与 `udp_writer` 已在本仓。
