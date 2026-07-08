# scripts/ — 8007 收端运维脚本

本目录脚本仅在 **收端（8007）** 使用：绑 vfio、启动 F-Stack 收包、退出后恢复内核网卡。  
发端（8004）脚本见 `sender/`。

> **状态：** v1.0.1 已落地（全 IPv6，收端 `…:0002`，UDP 8888）。本 README 与 `lib/link_profile.sh` / 各脚本实际行为一致。

---

## 双口分轨（82599 f0 / f1）

82599 双口 **职责分离**，setup / release **只动当前 `--link` 对应的那张卡**，互不干扰：

| 轨道 | `--link` | 内核接口 | PCI | 收端地址 | 对端 | UDP 端口 | F-Stack config | 典型场景 |
|------|----------|----------|-----|----------|------|----------|----------------|----------|
| **f1 / DAC** | `dac`（默认） | `enp66s0f1` | `0000:42:00.1` | IPv6 `1111:2222:…:0002/64` | 8004 `…:0003` | **8888** | `configs/config.dac.ini` | 8004↔8007 万兆 DAC 打流 |
| **f0 / 光纤** | `fibre` | `enp66s0f0` | `0000:42:00.0` | IPv6 `1111:2222:…:0002/64` | 交换板 FPGA `…:0001` | **8888** | `configs/config.fibre.ini` | 交换板 raw / 光纤口 |

**当前联调约定（1.0.1 起 dac/fibre 全 IPv6，收端统一 `…:0002`）：**

- dac（f1）与 fibre（f0）均走 IPv6，监听 **8888**；同一时刻只启用一条链路（`--link` 互斥）。
- dac 联调时 **不 down / 不 bind / 不 release f0**，f0 可保持内核态在线；反之亦然。
- `release_env.sh --link dac` 仅还回 **f1**：`ixgbe` + IPv6 `…:0002/64`；**不会**动 f0。
- `release_env.sh --link fibre` 仅还回 **f0**：`ixgbe` + IPv6 `…:0002/64` + MAC `90:e2:ba:50:aa:70`（与交换板 FPGA 一致）；**不会**动 f1。
- 旧 IPv4 `10.10.10.x` / `192.168.1.x` 联调地址已废弃。

---

## 脚本一览

| 脚本 | 用途 |
|------|------|
| `lib/link_profile.sh` | 被其他脚本 source；解析 `--link`，导出 PCI/IP/config 等变量 |
| `setup_env.sh` | 运行收包前：删内核 IP、down 口、绑 **vfio-pci**（**仅目标口**） |
| `release_env.sh` | 运行结束后：还回 **ixgbe**、up 口、恢复测试 IP（**仅目标口**） |
| `run_recv.sh` | **推荐入口**：setup → `fstack_udp_recv` → 退出时自动 release |

---

## `lib/link_profile.sh`

**用途：** 统一 `--link dac|fibre` 与路径变量，避免各脚本重复硬编码。

**用法（由其它脚本 source，一般不直接执行）：**

```bash
source "$(dirname "$0")/lib/link_profile.sh"
link_profile_apply dac   # 或 fibre
# 之后可使用：
#   $LINK_IFACE  $LINK_PCI  $LINK_IP  $LINK_PREFIX
#   $LINK_DST_PORT  $FSTACK_CONFIG  $FF_PATH  $DEVBIND
```

**可变参数 / 环境变量：**

| 名称 | 默认 | 说明 |
|------|------|------|
| `FF_PATH` | `/home/dev/HMH--8007/F-stack` | F-Stack 根目录（只读依赖） |
| `PROJECT_ROOT` | 自动推断 | `Fstack-DPDk` 根目录 |

**导出变量（`link_profile_apply` 之后）：**

| 变量 | dac（f1） | fibre（f0） |
|------|-----------|-------------|
| `LINK_IFACE` | `enp66s0f1` | `enp66s0f0` |
| `LINK_PCI` | `0000:42:00.1` | `0000:42:00.0` |
| `LINK_IPV6` | `1111:2222:…:0002` | `1111:2222:…:0002` |
| `LINK_PREFIX` | `64` | `64` |
| `LINK_MAC` | `90:e2:ba:50:aa:71`（f1） | `90:e2:ba:50:aa:70`（f0，与交换板一致） |
| `LINK_REMOTE_IPV6` | `…:0003`（8004 发端） | `…:0001`（交换板） |
| `LINK_DST_PORT` | **8888** | **8888** |
| `FSTACK_CONFIG` | `configs/config.dac.ini` | `configs/config.fibre.ini` |

---

## `setup_env.sh`

**用途：** 将 **指定** 万兆口从内核交给 DPDK（vfio），供 F-Stack 使用。**不操作另一张口。**

**用法：**

```bash
cd /home/dev/HMH--8007/Fstack-DPDk
sudo ./scripts/setup_env.sh [--link dac|fibre]
```

**可变参数：**

| 参数 | 默认 | 说明 |
|------|------|------|
| `--link` | `dac` | 见上表；`dac` 只绑 f1，`fibre` 只绑 f0 |

**行为（以 `--link dac` 为例，仅 f1）：**

1. `modprobe vfio-pci`
2. `ip -6 addr del 1111:…:0002/64` / `ip link set down`（**仅** `enp66s0f1`）
3. `dpdk-devbind.py --bind=vfio-pci 0000:42:00.1`
4. **不** down / bind `enp66s0f0`（f0 保持内核态）

**注意：** 执行后内核中 **看不到** 被绑定的接口（如 `ip link` 无 `enp66s0f1` 属正常）；另一口仍可见。

---

## `release_env.sh`

**用途：** F-Stack / 收包进程退出后，把 **指定口** 还回内核并恢复联调地址。

**用法：**

```bash
sudo ./scripts/release_env.sh [--link dac|fibre]
```

**可变参数：**

| 参数 | 默认 | 说明 |
|------|------|------|
| `--link` | `dac` | 与 setup 使用同一 profile |

**行为（以 `--link dac` 为例，仅 f1）：**

1. `dpdk-devbind.py --bind=ixgbe 0000:42:00.1`
2. `ip link set enp66s0f1 up`
3. `ip -6 addr add 1111:…:0002/64 dev enp66s0f1`（若尚未配置）
4. **不** 修改 f0 的 IPv6 / 驱动状态

**行为（`--link fibre`，仅 f0）：**

1. `dpdk-devbind.py --bind=ixgbe 0000:42:00.0`
2. `ip link set enp66s0f0 up`
3. 恢复 IPv6 `…:0002/64` + 设置 MAC `90:e2:ba:50:aa:70`（与交换板 FPGA 一致）
4. **不** 修改 f1 的 IPv6 / 驱动状态

---

## `run_recv.sh`

**用途：** 8007 **一键 live 收包 + 落盘**；内部调用 `setup_env.sh`，启动 `build/fstack_udp_recv`，**Ctrl+C 或正常退出时自动 `release_env.sh`**。

**用法：**

```bash
# 构建（首次）
cmake -S . -B build && cmake --build build -j"$(nproc)"

# f1 / DAC 双端打流（默认）
sudo ./scripts/run_recv.sh --link dac --raw-payload \
    --output test/output/live_dac.bin

# f0 / 光纤 / 交换板 raw
sudo ./scripts/run_recv.sh --link fibre --raw-payload --payload-size 1344 \
    --output test/output/live_fibre.bin

# 收满 N 包后自动退出并校验（可选）
sudo ./scripts/run_recv.sh --link dac --expected-pkts 10000 \
    --raw-payload --output test/output/fixed_count.bin
```

**脚本参数（传给编排层）：**

| 参数 | dac 默认 | fibre 默认 | 说明 |
|------|----------|------------|------|
| `--link` | `dac` | — | `dac` \| `fibre` |
| `--output PATH` | `test/output/live.bin` | 同左 | 落盘文件路径（相对项目根） |
| `--dst-port PORT` | **8888** | **8888** | UDP 绑定端口（F-Stack `ff_bind`） |
| `--raw-payload` | 默认开启 | 同左 | 透传整段 UDP payload，不解析 8B 头 |
| `--no-raw-payload` | — | — | 解析 8B 头（`seq+len`）后落 data 区 |
| `--payload-size N` | `1344` | 同左 | raw 模式标称长度，仅统计 mismatch；`0`=不检查 |
| `--expected-pkts N` | `0` | 同左 | 收满 N 个匹配包后退出；`0`=持续运行至 Ctrl+C |
| `--rx-cpu ID` | `0` | 同左 | F-Stack / RX 线程倾向 CPU（`ff_run`） |
| `--writer-cpu ID` | `1` | 同左 | writer pthread 绑核 |
| `--no-restore` | — | — | 退出时不 release（调试 vfio 状态时用） |
| `-h`, `--help` | — | — | 打印帮助 |

**透传给 `fstack_udp_recv` 的参数**（由 `run_recv.sh` 组装，一般无需手敲）：

| 应用参数 | 说明 |
|----------|------|
| `-c <config.ini>` | 由 `--link` 选择 `configs/config.*.ini` |
| `--output` | 同脚本 `--output` |
| `--dst-port` | 同脚本 `--dst-port`（dac / fibre 均 **8888**） |
| `--expected-pkts` | 同脚本 `--expected-pkts` |
| `--raw-payload` / `--no-raw-payload` | 同脚本 |
| `--payload-size` | 同脚本 `--payload-size` |

**环境变量：**

| 变量 | 默认 | 说明 |
|------|------|------|
| `FF_PATH` | `/home/dev/HMH--8007/F-stack` | F-Stack 安装路径 |
| `FSTACK_RECV_BIN` | `build/fstack_udp_recv` | 收端二进制路径 |

**联调顺序（dac / f1）：**

1. 8007：`sudo ./scripts/run_recv.sh --link dac ...`（等待 READY / 监听 **8888**）
2. 8004：`cd sender && sudo bash run_pktgen_sender.sh --to-8007 ...`（发向 `…:0002:8888`，本端 `…:0003`）
3. 8007：Ctrl+C 停止 → 自动 release **f1**（IPv6 `…:0002/64`）→ 可选 `test/verify_output.sh`  
   f0 全程保持内核态，无需 release。

---

## 与 receiver_dpdk 脚本对照

| 场景 | F-Stack（本目录） | receiver_dpdk |
|------|-------------------|---------------|
| f1 / DAC 打流 | `run_recv.sh --link dac` | `Test/run_udp_dpdk_live.sh`（默认 f1） |
| f0 / 光纤 | `run_recv.sh --link fibre` | `docs/run_live_test.sh` |
| dac 监听端口 | **8888**（1.0.1 起） | **50000**（旧 IPv4 路径，`UDP_DST_PORT`） |
| dac 还网卡 | `release_env.sh --link dac` → f1 IPv6 `…:0002/64` | `run_udp_dpdk_live.sh` trap → 旧 IPv4 |
| f0 是否参与 dac 联调 | **否**（不 bind / 不 release f0） | **否** |

---

## 常见问题

| 现象 | 处理 |
|------|------|
| `No such device: enp66s0f1` | f1 仍在 vfio，执行 `release_env.sh --link dac` |
| 8004 发流不通 | 确认 `--link dac` 与发端 `--to-8007`；8004 源 `…:0003` → 8007 目的 `…:0002:8888` |
| 端口不匹配 | dac / fibre 均 **8888**；若发端用旧 50000，改 sender 的 `--dst-port` 或 sender_config.h |
| 忘记 release | 优先用 `run_recv.sh`（带 trap）；手动跑过二进制时用对应 `--link` 的 `release_env.sh` |
| dac 联调后 f0 异常 | dac 脚本不应动 f0；若 f0 被误绑 vfio，用 `release_env.sh --link fibre` 单独恢复 |
