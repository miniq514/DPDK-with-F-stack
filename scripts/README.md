# scripts/ — 收端运维脚本

本目录为 **收端** 脚本：绑 DPDK 驱动、启动 F-Stack 收包、退出后恢复内核网卡。发端见 `sender/`。

**启动命令见 [../command.md](../command.md)。** 文档与 8004/8007 同步（2026-07-14）。

> **状态：** V1.1+。dac/fibre 全 IPv6。以各机 `lib/link_profile.sh` 为准（**勿用 8004 的 profile 覆盖 8007**，IP 角色相反）。

### 与 8007 部署的差异

| 项目 | 8004 收端（本仓库常见） | 8007 收端 |
|------|-------------------------|-----------|
| bind IPv6 | `…:0003` | `…:0002` |
| DPDK 驱动 | `igb_uio` + 运行时 IOMMU identity | 常 `vfio-pci`（内核 `iommu=pt`）或本机 profile |
| IOMMU 组 | f0+f1 一并绑 | 视内核/驱动可单口 |
| 工程路径 | `/home/cics/HMH/Fstack-DPDk_8004` | `/home/dev/HMH--8007/Fstack-DPDk` |
| `FF_PATH` | `/home/cics/HMH/F-stack` | `/home/dev/HMH--8007/F-stack` |

---

## 双口分轨（82599 f0 / f1）

82599 双口 f0/f1 **常同属一块 IOMMU group**。8004 上 `vfio-pci` 往往无法使组 viable，故 `setup_env.sh` 用 **`igb_uio` 将同组 f0+f1 一并绑给 DPDK**；`release_env.sh` 一并还回 `ixgbe`，但 **仅目标口恢复 IPv6**。

| 轨道 | `--link` | 内核接口 | PCI | 8004 收端地址 | 8007 收端地址 | UDP | 典型场景 |
|------|----------|----------|-----|---------------|---------------|-----|----------|
| **f1 / DAC** | `dac`（默认） | `enp66s0f1` | `0000:42:00.1` | `…:0003` | `…:0002` | 8888 | 当前默认：8007 发 → 8004 收 |
| **f0 / 光纤** | `fibre` | `enp66s0f0` | `0000:42:00.0` | （本机配置） | `…:0002` | 8888 | 交换板 → 8007 f0 |

**联调约定：**

- 同一时刻只启用一条链路（`--link` 互斥）。
- 8004 dac：f0 也会绑 `igb_uio`，但不配 f0 IPv6；release 后恢复 **f1** `…:0003`。
- 旧 IPv4 联调地址已废弃。

---
## 脚本一览

| 脚本 | 用途 |
|------|------|
| `lib/link_profile.sh` | 被其他脚本 source；解析 `--link`，导出 PCI/IP/config/DPDK 驱动变量 |
| `lib/hugepage.sh` | hugepages 继承检查（`setup_env` 调用） |
| `setup_env.sh` | 运行收包前：IOMMU identity、hugepages、down 口、绑 **`igb_uio`**（同组 f0+f1） |
| `release_env.sh` | 运行结束后：还回 **ixgbe**（同组 f0+f1）、up 口、恢复目标口 IPv6 |
| `run_recv.sh` | **推荐入口**：setup → `fstack_udp_recv` → 退出时自动 release |

---

## `lib/link_profile.sh`

**用途：** 统一 `--link dac|fibre` 与路径变量，避免各脚本重复硬编码。

**用法（由其它脚本 source，一般不直接执行）：**

```bash
source "$(dirname "$0")/lib/link_profile.sh"
link_profile_apply dac   # 或 fibre
# 之后可使用：
#   $LINK_IFACE  $LINK_PCI  $LINK_SIBLING_IFACE  $LINK_SIBLING_PCI
#   $LINK_IPV6  $LINK_PREFIX  $LINK_DST_PORT
#   $FSTACK_CONFIG  $FF_PATH  $DEVBIND  $DPDK_BIND_DRIVER
```

**可变参数 / 环境变量：**

| 名称 | 默认 | 说明 |
|------|------|------|
| `FF_PATH` | `/home/cics/HMH/F-stack` | F-Stack 根目录 |
| `DPDK_BIND_DRIVER` | `igb_uio` | DPDK 绑定驱动 |
| `DPDK_KERNEL_DRIVER` | `ixgbe` | release 时还回的内核驱动 |
| `PROJECT_ROOT` | 自动推断 | 项目根目录 |

**导出变量（`link_profile_apply` 之后，dac 为例）：**

| 变量 | dac（f1 目标） | fibre（f0 目标） |
|------|----------------|------------------|
| `LINK_IFACE` | `enp66s0f1` | `enp66s0f0` |
| `LINK_PCI` | `0000:42:00.1` | `0000:42:00.0` |
| `LINK_SIBLING_IFACE` | `enp66s0f0` | `enp66s0f1` |
| `LINK_SIBLING_PCI` | `0000:42:00.0` | `0000:42:00.1` |
| `LINK_IPV6` | `1111:2222:…:0003` | `1111:2222:…:0003` |
| `LINK_PREFIX` | `64` | `64` |
| `LINK_MAC` | `90:e2:ba:50:7c:f9`（8004 f1） | `90:e2:ba:50:aa:70`（交换板一致） |
| `LINK_REMOTE_IPV6` | `…:0002`（8007 发端） | `…:0001`（交换板） |
| `LINK_DST_PORT` | **8888** | **8888** |
| `FSTACK_CONFIG` | `configs/config.dac.ini` | `configs/config.fibre.ini` |

---

## `setup_env.sh`

**用途：** 将万兆口从内核交给 DPDK（`igb_uio`），供 F-Stack 收端使用。因 IOMMU 组约束，**f0 与 f1 一并绑定**。

**用法：**

```bash
cd /home/cics/HMH/Fstack-DPDk_8004
sudo ./scripts/setup_env.sh [--link dac|fibre]
# 或指定 F-Stack 路径：
sudo FF_PATH=/home/cics/HMH/F-stack ./scripts/setup_env.sh --link dac
```

**可变参数：**

| 参数 | 默认 | 说明 |
|------|------|------|
| `--link` | `dac` | `dac` 以 f1 为收流口；`fibre` 以 f0 为收流口 |

**行为（以 `--link dac` 为例）：**

1. `ensure_iommu_identity`：将 IOMMU group 从 Translated 切为 **identity**（8004 无 `iommu=pt`，否则 DPDK DMA 被拦截）
2. `hugepages_ensure_inherit`：检查 hugepages
3. down 目标口 `enp66s0f1`，删 IPv6 `…:0003/64`
4. `modprobe igb_uio`
5. 绑定同组 **f0**（`0000:42:00.0`）+ **f1**（`0000:42:00.1`）→ `igb_uio`

**注意：** 执行后内核中 **看不到** 被绑定的万兆口（`ip link` 无 `enp66s0f*` 属正常）。

> 永久修复 IOMMU：在 `/etc/default/grub` 的 `GRUB_CMDLINE_LINUX` 添加 `iommu=pt`，然后 `update-grub` + 重启。

---

## `release_env.sh`

**用途：** F-Stack / 收包进程退出后，把 **同 IOMMU 组 f0+f1** 还回内核，并在 **目标口** 恢复联调 IPv6。

**用法：**

```bash
sudo ./scripts/release_env.sh [--link dac|fibre]
```

**行为（`--link dac`，目标 f1）：**

1. f1 + f0 → `ixgbe`
2. `ip link set enp66s0f1 up`
3. 恢复 IPv6 `…:0003/64` dev `enp66s0f1`
4. `ip link set enp66s0f0 up`（同组口仅 up，不恢复 IP）

**行为（`--link fibre`，目标 f0）：**

1. f0 + f1 → `ixgbe`
2. `ip link set enp66s0f0 up`，设置 MAC `90:e2:ba:50:aa:70`
3. 恢复 IPv6 `…:0003/64` dev `enp66s0f0`
4. `ip link set enp66s0f1 up`（同组口仅 up）

---

## `run_recv.sh`

**用途：** **一键 live 收包 + 落盘**；内部调用 `setup_env.sh`，启动 `build/fstack_udp_recv`，**Ctrl+C 或正常退出时自动 `release_env.sh`**。

**用法：**

```bash
# 构建（首次）
cmake -S . -B build && cmake --build build -j"$(nproc)"

# f1 / DAC（8004 收端，当前默认）
sudo FF_PATH=/home/cics/HMH/F-stack ./scripts/run_recv.sh --link dac --raw-payload \
    --expected-pkts 100000 \
    --output test/output/live_dac.bin

# f0 / 光纤 / 交换板 raw
sudo ./scripts/run_recv.sh --link fibre --raw-payload --payload-size 1344 \
    --output test/output/live_fibre.bin
```

**脚本参数：**

| 参数 | dac 默认 | fibre 默认 | 说明 |
|------|----------|------------|------|
| `--link` | `dac` | — | `dac` \| `fibre` |
| `--output PATH` | `test/output/live_<link>.bin` | 同左 | 落盘文件路径（相对项目根） |
| `--dst-port PORT` | **8888** | **8888** | UDP 绑定端口（F-Stack `ff_bind`） |
| `--raw-payload` | 默认开启 | 同左 | 透传整段 UDP payload，不解析 8B 头 |
| `--no-raw-payload` | — | — | 解析 8B 头（`seq+len`）后落 data 区 |
| `--payload-size N` | `1344` | 同左 | raw 模式标称长度；`0`=不检查 |
| `--expected-pkts N` | `0` | 同左 | 收满 N 包后退出；`0`=持续至 Ctrl+C |
| `--rx-cpu ID` | `0` | 同左 | F-Stack / RX 线程倾向 CPU |
| `--writer-cpu ID` | `1` | 同左 | writer pthread 绑核 |
| `--no-restore` | — | — | 退出时不 release（调试 DPDK 绑定状态时用） |
| `-h`, `--help` | — | — | 打印帮助 |

**环境变量：**

| 变量 | 默认 | 说明 |
|------|------|------|
| `FF_PATH` | `/home/cics/HMH/F-stack` | F-Stack 安装路径 |
| `FSTACK_RECV_BIN` | `build/fstack_udp_recv` | 收端二进制路径 |

**联调顺序（dac，8007 发 → 8004 收，当前默认）：**

1. **8004**：`sudo FF_PATH=/home/cics/HMH/F-stack ./scripts/run_recv.sh --link dac --expected-pkts N ...`（监听 `…:0003:8888`）
2. **8007**：`cd sender && sudo bash run_pktgen_sender.sh --to-8004 --count N ...`（发向 `…:0003:8888`，源 `…:0002`）
3. **8004**：收满或 Ctrl+C → 自动 release f1（IPv6 `…:0003/64`）

反向（8004 发 / 8007 收）见 [command.md](../command.md) 场景 B。

---

## 与 receiver_dpdk 脚本对照

| 场景 | F-Stack（本目录） | receiver_dpdk |
|------|-------------------|---------------|
| f1 / DAC 打流 | `run_recv.sh --link dac` | `Test/run_udp_dpdk_live.sh`（默认 f1） |
| f0 / 光纤 | `run_recv.sh --link fibre` | `docs/run_live_test.sh` |
| dac 监听端口 | **8888**（V1.0.1 起） | **50000**（旧 IPv4 路径） |
| dac 还网卡 | `release_env.sh --link dac` → f1 IPv6 `…:0003/64` | `run_udp_dpdk_live.sh` trap → 旧 IPv4 |
| IOMMU 组处理 | f0+f1 一并绑 `igb_uio` | 视部署环境而定 |

---

## 常见问题

| 现象 | 处理 |
|------|------|
| `No such device: enp66s0f1` | f1 仍在 DPDK 驱动下，执行 `release_env.sh --link dac` |
| `VFIO group is not viable` | 8004 不要用 `vfio-pci`；用本仓库 `setup_env.sh`（`igb_uio`） |
| 包到网卡但收端 0 包 / DMA 失败 | IOMMU 仍为 Translated；检查 `setup_env` 是否切到 identity，或加 `iommu=pt` 重启 |
| 8007 发流不通 | 确认 8004 已 READY；8007 `--to-8004` 发向 `…:0003:8888` |
| 端口不匹配 | dac / fibre 均 **8888**；发端 `--dst-port` 须一致 |
| 忘记 release | 优先用 `run_recv.sh`（带 trap）；手动跑过二进制时用对应 `--link` 的 `release_env.sh` |
| release 后 f0 无 IP | 正常；dac 模式仅 f1 恢复 `…:0003/64`，f0 仅 up |
