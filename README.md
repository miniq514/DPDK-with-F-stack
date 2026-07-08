# Fstack-DPDk

**重要！：当前是8007**

**F-Stack UDP 收包 + io_uring 落盘 + sender pktgen 打流**，全程 IPv6（对齐 1.0.1）。
8007 为收端，8004 / 交换板为发端；支持 dac（双端万兆直连）与 fibre（交换板 raw 落盘）两条链路。
sender 工具包可通过 `--to-8004` / `--to-8007` 切换打流目标，便于反向/自环测试。

## 本端角色与使用方式（8004 / 8007 互换）

发端与收端可能在 8004、8007 之间来回切换。**两机 IP 固定不变**：8007 恒为 `…:0002`，8004 恒为 `…:0003`，与角色无关。下表给出本端在 4 种组合下应跑哪个程序、关键命令与对端配合。

| 本端 | 本端角色 | 本端程序 | 关键参数 | 对端角色 / 命令 |
|------|----------|----------|----------|------------------|
| **8007** | 收端 | `scripts/run_recv.sh` | `--link dac`（默认 bind `…:0002:8888`） | 8004 跑 sender：`--to-8007` |
| **8007** | 发端 | `sender/run_pktgen_sender.sh` | `--to-8004`（发向 `…:0003`，src 自动 `…:0002`） | 8004 跑收端（见下"收端跑在 8004"注意点） |
| **8004** | 发端 | `sender/run_pktgen_sender.sh` | `--to-8007`（发向 `…:0002`，src 自动 `…:0003`） | 8007 跑收端：`--link dac` |
| **8004** | 收端 | `scripts/run_recv.sh` | `--link dac`，**需先改 bind IP 为 `…:0003` 并重编**（见下） | 8007 跑 sender：`--to-8004` |

### 场景 A — 8007 收 / 8004 发（默认）

```bash
# 8007（收端，本端）
sudo ./scripts/run_recv.sh --link dac --raw-payload --expected-pkts 100000 \
    --output test/output/live_dac.bin

# 8004（发端，本端）
cd sender
sudo bash run_pktgen_sender.sh --to-8007 --count 100000 --rate-mbps 6000 --force-regen-pcap
```

### 场景 B — 8007 发 / 8004 收（反向 / 自环）

```bash
# 8004（收端，本端）—— 需先把收端 bind IP 改成 8004 的 …:0003（见注意点 1）
sudo ./scripts/run_recv.sh --link dac --raw-payload --expected-pkts 1000 \
    --output test/output/live_to8004.bin

# 8007（发端，本端）
cd sender
sudo bash run_pktgen_sender.sh --to-8004 --count 1000 --force-regen-pcap
```

### 注意点

1. **收端 bind IP 当前硬编码为 8007 的 `…:0002`**（`include/demo_udp_config.h:UDP_RX_BIND_IP6` 与 `configs/config.*.ini` 的 `addr6`）。**8004 跑收端前必须改为 `…:0003` 并重新编译**，否则 `ff_bind` 会绑错地址、收不到包：
   ```bash
   # 把 …:0002 改成 …:0003（两处必须同时改，保持一致）
   #   include/demo_udp_config.h:  #define UDP_RX_BIND_IP6  "...:0003"
   #   configs/config.dac.ini:     addr6=...:0003
   cmake --build build -j"$(nproc)"
   ```
   切回 8007 收端时改回 `…:0002` 重编。后续计划把 bind IP 挪入 `app_config`（`--bind-ip`）以消除重编。

2. **sender 灵活，无需重编**：`--to-8004`/`--to-8007` 切换目标，src IPv6/MAC 自动按目标反推（发往 8007 → 本机=8004 `…:0003`；发往 8004 → 本机=8007 `…:0002`），也可用 `--src-ip`/`--src-mac`/`--dst-mac` 覆盖。

3. **同一台机器同一时刻只跑一个角色**（sender 或 receiver），两者都要把 f1（`enp66s0f1` / `0000:42:00.1`）绑 DPDK 驱动，互斥；切换角色前先让上一个进程退出。`run_recv.sh` 和 `sender/run_pktgen_sender.sh` 现在都会在正常退出 / Ctrl+C / SIGTERM 后自动 release 网卡、恢复 IPv6。

4. **两机网卡名相同但 MAC 不同**：8007 f1 = `90:e2:ba:50:aa:71`，8004 f1 = `90:e2:ba:50:7c:f9`。sender pcap 的 dst MAC 由 `--to-*` 自动选对（`--to-8007` → aa:71，`--to-8004` → 7c:f9），无需手填。收端 DPDK 占用期间用网卡硬件 MAC，release 后 `release_env.sh` 会恢复 MAC。

5. **fibre 链路仅 8007 收**（发端为交换板 FPGA `…:0001`，不走 sender），8004 不参与 fibre；角色互换只在 dac 链路进行。

6. **时序**：收端先 READY → 发端再发包；发端 `--count` 须与收端 `--expected-pkts` 一致，否则收端会 idle 超时退出或持续等待。

7. **首次部署任一角色**都要先做一次 DPDK 环境初始化（绑 vfio + hugepages）：收端由 `run_recv.sh` 自动处理；发端用 `cd sender && sudo bash run_pktgen_sender.sh --setup-dpdk --pci 0000:42:00.1`。

## 目录结构

```
Fstack-DPDk/
├── CMakeLists.txt          # 全局编译入口（收端 + sender）
├── README.md               # 本文（项目总览）
├── src/
│   └── fstack_udp_recv.c   # F-Stack UDP 收包主程序（ff_run RX + pthread writer）
├── include/                # demo_udp_config.h / fstack_udp_recv.h / udp_payload.h / udp_rx_timing.h
├── io-uring/               # udp_writer（io_uring 落盘库）+ linux_udp_recv（基线对照）
├── sender/                 # 发端工具包（程序A pcap 生成 + 程序B pktgen 打流）
│   ├── gen_udp_pcap.c          # 程序A：IPv6 pcap 生成器
│   ├── run_pktgen_sender.sh    # 程序B：pktgen 一键打流（发包数量可调）
│   ├── scripts/run_pktgen_replay.sh
│   ├── include/sender_config.h # IPv6 地址/MAC/端口常量
│   ├── start/command.md        # 启动命令与参数说明
│   └── CMakeLists.txt
├── configs/                # F-Stack config.ini（IPv6）
│   ├── config.dac.ini          # f1 / 0000:42:00.1
│   └── config.fibre.ini        # f0 / 0000:42:00.0
├── scripts/                # 8007 收端运维脚本
│   ├── lib/link_profile.sh     # --link dac|fibre 变量导出
│   ├── setup_env.sh            # 绑 vfio（仅目标口）
│   ├── release_env.sh          # 还回 ixgbe + 恢复 IPv6（仅目标口）
│   ├── run_recv.sh             # 推荐入口：setup → 收包 → 自动 release
│   └── README.md
├── test/                   # 落盘输出与校验
│   ├── verify_output.sh
│   ├── internet/               # 内核态 IPv6 配网脚本（ping6 互通测试）
│   └── README.md
└── docs/                   # 设计与迭代文档
    ├── README.md               # 约束 / 版本路线 / sender 工具包说明
    └── 迭代计划.md
```

## 拓扑与地址表（1.0.1 全 IPv6）

| 角色 | 节点 | 接口 | IPv6 | UDP 端口 | MAC |
|------|------|------|------|----------|-----|
| 收端 | **8007** | f1（dac）/ f0（fibre） | `1111:2222:1111:2222:1111:2222:1111:0002` | 8888 | f1 `90:e2:ba:50:aa:71` / f0 `90:e2:ba:50:aa:70` |
| 发端 | **8004**（dac pktgen） | `enp66s0f1` | `…:0003` | 8888 | `90:e2:ba:50:7c:f9` |
| 发端 | 交换板 FPGA（fibre） | — | `…:0001` | 8888 | `11:11:11:11:11:11`（交换板侧） |

- **dac**：8004 sender pktgen → 8007 f1 收端（双端万兆 DAC 直连）
- **fibre**：交换板 FPGA → 8007 f0 收端（光口 raw 落盘，不走 sender）
- 同一时刻只启用一条链路（`--link dac` 与 `--link fibre` 互斥）
- 旧 IPv4 `192.168.1.x` / `10.10.10.x` 联调地址已废弃

## 构建

```bash
cd /home/dev/HMH--8007/Fstack-DPDk
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

产物：
- `build/fstack_udp_recv` — 收端主程序
- `build/sender/gen_udp_pcap` — 程序A（IPv6 pcap 生成器）
- `build/libudp_writer.a` — io_uring 落盘库

依赖：`/home/dev/HMH--8007/F-stack`（`libfstack.a` + DPDK dpdk-install）、`liburing`、`pkg-config`。

## 快速开始

### 收端（8007）

```bash
# dac 链路（f1），raw 透传落盘
sudo ./scripts/run_recv.sh --link dac --raw-payload \
    --output test/output/live_dac.bin

# 收满 N 包自动退出（与发端 --count 一致）
sudo ./scripts/run_recv.sh --link dac --raw-payload --expected-pkts 100000 \
    --output test/output/fixed_count.bin

# fibre 链路（f0，交换板发流）
sudo ./scripts/run_recv.sh --link fibre --raw-payload --payload-size 1344 \
    --output test/output/live_fibre.bin
```

`run_recv.sh` 自动：setup_env（绑 vfio）→ `fstack_udp_recv -c configs/config.*.ini -- <app args>` → Ctrl+C 退出时自动 release_env（还回 ixgbe + 恢复 IPv6）。

### 发端（8004，sender）

```bash
cd sender

# 冒烟 100 包 → 8007
sudo bash run_pktgen_sender.sh --to-8007 --count 100 --force-regen-pcap

# 10 万包，6 Gbps
sudo bash run_pktgen_sender.sh --to-8007 --count 100000 --rate-mbps 6000 --force-regen-pcap

# 线速 200 万包
sudo bash run_pktgen_sender.sh --to-8007 --count 2000000 --rate 100 --force-regen-pcap

# 首次部署：绑 vfio + hugepages
sudo bash run_pktgen_sender.sh --setup-dpdk --pci 0000:42:00.1
```

**时序：收端先 READY → 发端再发包。`--count` 须与收端 `--expected-pkts` 一致。**

### 反向 / 自环（→ 8004）

发端在 8007，收端在 8004：

```bash
# 8004 上启动收端（假设 8004 也部署了 Fstack-DPDk）
sudo ./scripts/run_recv.sh --link dac --raw-payload --expected-pkts 1000 \
    --output test/output/live_to8004.bin

# 8007 上发端
cd sender
sudo bash run_pktgen_sender.sh --to-8004 --count 1000 --force-regen-pcap
```

### 程序A：单独生成 pcap（不发包）

```bash
# --payload-size 指数据区字节；raw 模式下 UDP payload = 8B头 + 1336B体 = 1344B
./build/sender/gen_udp_pcap udp_test.pcap --to-8007 --count 1000 --payload-size 1336
./build/sender/gen_udp_pcap udp_test.pcap --to-8004 --count 1000
```

## 落盘校验

```bash
# 仅检查文件存在 + 大小 + SHA256
bash test/verify_output.sh test/output/live_dac.bin

# 收满 N 包、每包 1344B raw，expected = N * 1344
bash test/verify_output.sh test/output/fixed_count.bin $((100000 * 1344))
```

## 文档索引

- [docs/README.md](docs/README.md) — 约束、版本路线、sender 工具包详细说明
- [docs/迭代计划.md](docs/迭代计划.md) — 版本里程碑与任务清单
- [scripts/README.md](scripts/README.md) — 收端运维脚本（setup/release/run_recv）
- [test/README.md](test/README.md) — 落盘输出与校验
- [test/internet/README.md](test/internet/README.md) — 内核态 IPv6 配网与 ping6 互通
- [sender/start/command.md](sender/start/command.md) — 发端打流命令示例

## 依赖（只读，不修改）

| 路径 | 用途 |
|------|------|
| `/home/dev/HMH--8007/F-stack` | `libfstack.a`、DPDK dpdk-install |
| `/home/dev/HMH--8007/receiver_dpdk` | 拷贝来源（io_uring、include、RX/writer 逻辑） |
| `/home/dev/HMH--8007/others/peer_sender` | 参考（已迁移为 `sender/`，IPv6 + to-8004/to-8007） |

## 收端架构

```
ff_run(rx_loop)  ──ff_recvfrom──►  rx_extract_payload  ──►  rx_enqueue_payload
   (RX 线程)                                                       │
                                                                   ▼
                                                              rte_ring (sp/sc)
                                                                   │
                                                                   ▼
                                              writer_thread_main ──► udp_writer_submit
                                                 (pthread)              │
                                                                        ▼
                                                                  io_uring 落盘 .bin
```

- raw 模式：整段 UDP payload 透传落盘（交换板 1344B 流）
- 非 raw 模式：剥离 8B 头（seq+len）后落 data 区
- 收满 `--expected-pkts` 或 idle `UDP_RX_IDLE_EXIT_SEC` 秒无新包自动退出

## 更新日志

| 日期 | 改动 |
|------|------|
| 2026-07-08 | 新增 `sender/` 工具包：程序A `gen_udp_pcap.c`（IPv6 pcap 生成器）、程序B `run_pktgen_sender.sh`（pktgen 打流，发包数量可调），均支持 `--to-8004`/`--to-8007` 切换目标。 |
| 2026-07-08 | 根 `CMakeLists.txt` 加 `add_subdirectory(sender)`，全局编译含 `gen_udp_pcap`。 |
| 2026-07-08 | 实现 `src/fstack_udp_recv.c` 的 `rx_loop`（ff_recvfrom 非阻塞收包 + 退出判定）与 `run_recv_pipeline`（RX/writer 双线程编排）；清理 stale TODO 与 format 警告。 |
| 2026-07-08 | 新建 `configs/config.dac.ini` 与 `config.fibre.ini`（全 IPv6，收端 `…:0002`）。 |
| 2026-07-08 | 新建 `scripts/lib/link_profile.sh`、`setup_env.sh`、`release_env.sh`、`run_recv.sh`（收端一键联调，退出自动 release）。 |
| 2026-07-08 | 更新 `docs/README.md`：新增 sender 工具包章节，全文 `peer/` → `sender/`，dac 数据流改 IPv6。 |
| 2026-07-08 | 更新 `scripts/README.md`：dac/fibre 统一 IPv6 + 端口 8888，移除 IPv4 联调假设。 |
| 2026-07-08 | 新建项目根 `README.md`（本文）。 |
| 2026-07-08 | 新增 `test/self_loop_pcap.sh` + `self_loop_pcap_check.py`：pcap 自环测试（程序A 生成 → 收端 `udp_payload.h` 解析逻辑回读校验）。修复 `gen_udp_pcap.c` UDP checksum 字节序 bug（`calc_chksum` 按 big-endian 求和后赋值 `udp.chksum` 缺 `htons`，小端机上 wire 字节被反转，会导致 F-Stack 收端校验失败丢包）；自环测试覆盖 EtherType/IPv6/UDP 端口/checksum/payload seq 与递增模式，`--to-8007`/`--to-8004`、奇/偶 payload、5000 包均通过。 |
| 2026-07-08 | README 开头新增「本端角色与使用方式」章节：4 种组合（8007/8004 × 收/发）的命令与对端配合，并明确注意点——收端 bind IP 硬编码 `…:0002`，8004 跑收端须改 `UDP_RX_BIND_IP6` + `configs/config.*.ini` 的 `addr6` 为 `…:0003` 并重编；sender 灵活无需重编。 |
|| 2026-07-08 | 对齐 8004 稳定 sender：修正 `sender/include/sender_config.h` 的 `SEND_PAYLOAD_SIZE` 为 1336（数据区），使 raw 模式 UDP payload = 8B头 + 1336B体 = 1344B，与收端 `UDP_RX_PAYLOAD_SIZE` 一致；`run_pktgen_sender.sh` 增加 `--no-release` 选项，并在正常退出 / Ctrl+C / SIGTERM 后自动把发端网卡还回 ixgbe、恢复 IPv6。 |
