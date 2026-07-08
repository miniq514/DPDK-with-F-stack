# Fstack-DPDk

`receiver_dpdk` → F-Stack 收包迁移工程。**落盘（io_uring）不变**，收包走 F-Stack UDP socket。
**8004** 仅作 **`--link dac` 发端**；**8007** 为收端。`--link fibre` 发端为 **交换板 FPGA**（非 8004）。

## 版本路线


| 版本      | 内容                                                                                                           |
| ----------- | ---------------------------------------------------------------------------------------------------------------- |
| 1.0       | F-Stack UDP + RX/writer 双线程 + io_uring 落盘（live）；dac 可暂用 IPv4 验证                                   |
| **1.0.1** | **全链路统一 IPv6**；8007 收端 `…:0002` 与 fibre 对齐；f0 MAC 与交换板一致（见 [迭代计划.md](./迭代计划.md)） |
| 2.0       | + TCP                                                                                                          |
| 3.0       | + KNI                                                                                                          |
| 4.0       | UDP 速率不达标时，收包前端切回原生 DPDK 通道                                                                   |

## 网口（脚本 `--link` 切换）


| `--link` | 口          | 协议                                        | 收端 (8007)                     | 发端                                | 用途            |
| ---------- | ------------- | --------------------------------------------- | --------------------------------- | ------------------------------------- | ----------------- |
| `dac`    | `enp66s0f1` | IPv4（**1.0 暂用**）→ **IPv6（1.0.1 起）** | `192.168.1.2` → `1111:…:0002` | **8004** `192.168.1.3` → `…:0003` | 双端万兆打流    |
| `fibre`  | `enp66s0f0` | **IPv6**                                    | `1111:…:0002`                  | **交换板 FPGA** `…:0001`           | 交换板 raw 落盘 |

> **1.0.1 目标：** dac / fibre **全部 IPv6**；8007 收端统一 `1111:2222:1111:2222:1111:2222:1111:0002`。详见 [迭代计划.md](./迭代计划.md)。

发端 **pktgen** 仅用于 **`--link dac`**。v1.0 仅 live，pcap 离线回归后续再做。

---

## 约束

本节为联调与实现的**硬性约定**；脚本、`config.*.ini`、收端程序均不得与下表冲突。

### `dac`（双端打流）


| 项       | 约束（**1.0.1 起与 fibre 同为 IPv6**）                                                           |
| ---------- | -------------------------------------------------------------------------------------------------- |
| 协议     | **1.0.1 起必须 IPv6**；1.0 可暂用 IPv4 验证                                                      |
| 收端     | 8007`enp66s0f1`；**1.0.1：** `1111:2222:1111:2222:1111:2222:1111:0002/64`（与 fibre 收端相同）   |
| 发端     | **8004** `enp66s0f1`；**1.0.1：** `1111:2222:1111:2222:1111:2222:1111:0003/64`；`sender/` + pktgen |
| 目的 MAC | pktgen pcap DST MAC = 8007**f1** MAC（`90:e2:ba:50:aa:71`）                                      |
| 禁止     | 1.0.1 起禁止使用`192.168.1.x` 作为联调地址                                                       |

### `fibre`（交换板）


| 项        | 约束                                                                                |
| ----------- | ------------------------------------------------------------------------------------- |
| 协议      | **必须 IPv6**；**禁止** 使用 IPv4（含 `10.10.10.x`）作为交换板联调地址              |
| 发端      | **交换板 FPGA**（烧录固定，非 8004）；**不得** 用 `sender/` pktgen 代替交换板测 fibre |
| 收端口    | 8007**`enp66s0f0`**（`0000:42:00.0`，光口）                                         |
| 收端 IPv6 | `1111:2222:1111:2222:1111:2222:1111:0002`（`config.fibre.ini` / `ff_bind`）         |
| 发端 IPv6 | `1111:2222:1111:2222:1111:2222:1111:0001`                                           |
| UDP 端口  | **8888**（`0x22b8`），源/目的均为 8888                                              |
| 目的 MAC  | `90:e2:ba:50:aa:70`（f0；与交换板一致；必要时本机 f0 改 MAC 适配 FPGA）             |
| 源 MAC    | `11:11:11:11:11:11`（交换板侧，仅作识别）                                           |
| 以太类型  | `0x86dd`（IPv6）                                                                    |
| 禁止      | fibre 脚本/配置中出现`10.10.10.1`、`10.10.10.2` 等 IPv4 联调假设                    |

#### 交换板 UDP 包头关键字段（FPGA 参考）


| 字段         | 值                                        | 说明                     |
| -------------- | ------------------------------------------- | -------------------------- |
| Dst_Mac_Addr | `90:e2:ba:50:aa:70`                       | 目的 MAC（本机 f0）      |
| Src_Mac_Addr | `11:11:11:11:11:11`                       | 源 MAC（交换板）         |
| Ether_Type   | `0x86dd`                                  | 以太类型 = IPv6          |
| Version      | `6`                                       | IP 版本 = IPv6           |
| Src_Address  | `1111:2222:1111:2222:1111:2222:1111:0001` | 源 IPv6（发端 / 交换板） |
| Dst_Address  | `1111:2222:1111:2222:1111:2222:1111:0002` | 目的 IPv6（收端 / 8007） |
| Udp_Src_Port | `0x22b8`（8888）                          | 源 UDP 端口              |
| Udp_Dst_Port | `0x22b8`                                  |                          |

## v1.0 数据流

**dac：**

```
8004 pktgen ──IPv6 UDP:8888──► 8007 f1 F-Stack (ff_recvfrom) ──► ring ──► writer ──► 落盘
```

**fibre：**

```
交换板 FPGA ──IPv6 UDP:8888──► 8007 f0 F-Stack (ff_recvfrom) ──► ring ──► writer ──► 落盘
```

## sender 工具包

`sender/` 为发端（8004 或自环/反向 8007）的打流工具包，**全程 IPv6**，与收端 `fstack_udp_recv` 落盘口径一致。**不**直接拷贝 `others/peer_sender`，而是重写为 IPv6 + `--to-8004`/`--to-8007` 目标切换。

### 目录结构

```
sender/
├── CMakeLists.txt            ← 由根 CMake add_subdirectory(sender) 引入
├── gen_udp_pcap.c            ← 程序A：IPv6 pcap 生成器
├── run_pktgen_sender.sh      ← 程序B：pktgen 一键打流入口（发包数量可调）
├── scripts/
│   └── run_pktgen_replay.sh  ← pktgen 底层编排
├── include/
│   └── sender_config.h       ← IPv6 地址/MAC/端口常量
└── start/
    └── command.md            ← 启动命令与参数说明
```

### 程序A — `gen_udp_pcap.c`（IPv6 pcap 生成器）

生成与收端 `udp_payload.h` 解析一致的 IPv6 UDP pcap，供 pktgen 重放或自环测试。EtherType `0x86dd`，UDP checksum 按 IPv6 pseudo header 计算。

| 参数 | 默认 | 说明 |
|------|------|------|
| `--to-8007` | 默认 | dst IP `…:0002`、dst MAC `90:e2:ba:50:aa:71`（8007 f1） |
| `--to-8004` | — | dst IP `…:0003`、dst MAC `90:e2:ba:50:7c:f9`（8004 f1） |
| `--count N` | `100` | 包数 |
| `--payload-size B` | `1336` | 数据区字节；raw 模式下 UDP payload = 8B头 + 1336B体 = 1344B |
| `--src-ip IP` | 按目标反推 | 源 IPv6（发往 8007 默认本机=8004 `…:0003`） |
| `--src-mac MAC` | 按目标反推 | 源 MAC |
| `--dst-port PORT` | `8888` | 与收端 bind 一致 |
| `--src-port PORT` | `50001` | — |

### 程序B — `run_pktgen_sender.sh`（pktgen 打流，发包数量可调）

底层等价 `scripts/run_pktgen_replay.sh`；自动编译 `gen_udp_pcap` 并生成 pcap（除非 `--no-regen-pcap`）。

| 参数 | 默认 | 说明 |
|------|------|------|
| `--to-8007` | 默认 | 打流目标 8007（dst `…:0002`） |
| `--to-8004` | — | 打流目标 8004（dst `…:0003`，反向/自环） |
| `--count N` | `100` | **发包数量，可调**（与收端 `--expected-pkts` 一致） |
| `--payload-size B` | `1336` | 数据区字节；raw 下 UDP payload = 8B头 + 1336B体 = 1344B |
| `--rate PERCENT` | `100` | 线路速率百分比，`100`=线速 |
| `--rate-mbps MBPS` | — | 目标线路带宽 Mbps（与 `--rate` 二选一） |
| `--duration SEC` | — | 按时长发包（与 `--count` 二选一） |
| `--line-gbps N` | `10` | 网口标称线速，供 `--rate-mbps` 换算 |
| `--pci ADDR` | 自动解析 | 发端万兆口 PCI |
| `--bind-if NAME` | `enp66s0f1` | 发端口名（绑 vfio 前） |
| `--setup-dpdk` | — | 绑 vfio + hugepages（首次部署） |
| `--check` | — | 仅环境检查 |
| `--gen-pcap` | — | 仅生成 pcap，不发包 |
| `--force-regen-pcap` | — | 强制重建 pcap |
| `--no-regen-pcap` | — | 不自动生成 pcap |
| `--no-release` | — | 打流结束后不自动还网卡/恢复 IPv6 |

### 与收端配合时序

1. **收端先 READY**：8007 `sudo ./scripts/run_recv.sh --link dac --raw-payload --output test/output/live_dac.bin`
2. **发端打流**：8004 `cd sender && sudo bash run_pktgen_sender.sh --to-8007 --count 100000 --rate-mbps 6000`
3. 自环/反向：`--to-8004`（发端在 8007，收端在 8004）
4. **正常退出 / Ctrl+C / SIGTERM 后**，`run_pktgen_sender.sh` 会自动把发端网卡还回 `ixgbe` 并恢复本机 IPv6，避免下次启动时链路 Down。如需保留 DPDK 绑定（例如连续测试），加 `--no-release`。

### 目标切换语义

| 参数 | dst IPv6 | dst MAC | 场景 |
|------|----------|---------|------|
| `--to-8007` | `…:0002` | 8007 f1 `aa:71` | 正常：发端(8004) → 收端(8007) |
| `--to-8004` | `…:0003` | 8004 f1 `7c:f9` | 反向/自环：发端(8007) → 收端(8004) |

## 构建（8007，待实现后）

```bash
cd /home/dev/HMH--8007/Fstack-DPDk
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

## 联调（8007 live，待实现后）

```bash
# dac：8007 收 + 8004 发
./scripts/run_recv.sh --link dac --raw-payload --output test/output/live_dac.bin
# 8004: cd sender && ./run_pktgen_sender.sh --to-8007 --count 100000 --rate-mbps 6000

# fibre：8007 收 + 交换板发（IPv6，见「约束 · fibre」）
./scripts/run_recv.sh --link fibre --raw-payload --output test/output/live_fibre.bin
```

## 依赖（只读，不修改）


| 路径                                     | 用途                                          |
| ------------------------------------------ | ----------------------------------------------- |
| `/home/dev/HMH--8007/F-stack`            | `libfstack.a`、DPDK dpdk-install              |
| `/home/dev/HMH--8007/receiver_dpdk`      | 拷贝来源（io-uring、include、RX/writer 逻辑） |
| `/home/dev/HMH--8007/others/peer_sender` | 参考（已迁移为 `sender/`，IPv6 + to-8004/to-8007） |
