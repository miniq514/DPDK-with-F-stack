# 发端打流命令（sender）

工作目录：

```bash
cd ~/Fstack-DPDk/sender
```

**拓扑（IPv6，对齐 docs 1.0.1）：**
- 8007 收端 `1111:2222:1111:2222:1111:2222:1111:0002`，UDP **8888**
- 8004 发端 `1111:2222:1111:2222:1111:2222:1111:0003`
- 交换板 `…:0001`（fibre，不走 sender pktgen）

**时序：** 收端先 READY → 发端再发包。`--count` 须与收端 `--expected-pkts` 一致。

**退出与网卡释放：** 正常结束、Ctrl+C 或 SIGTERM 后，`run_pktgen_sender.sh` 会自动把发端网卡还回 `ixgbe` 并恢复本机 IPv6。如需保留 DPDK 绑定（连续测试），加 `--no-release`。

---

## 目标切换

| 参数 | dst IPv6 | dst MAC | 场景 |
|------|----------|---------|------|
| `--to-8007` | `…:0002` | 8007 f1 `90:e2:ba:50:aa:71` | 正常：发端(8004) → 收端(8007) |
| `--to-8004` | `…:0003` | 8004 f1 `90:e2:ba:50:7c:f9` | 反向/自环：发端(8007) → 收端(8004) |

src IP / src MAC 默认按目标反推（发往 8007 → 本机=8004 `…:0003`）。

---

## Part 1 — 冒烟测试（100 包，1344 B，不限速，→ 8007）

收端（8007）：

```bash
cd ~/Fstack-DPDk
sudo ./scripts/run_recv.sh --link dac --raw-payload \
    --expected-pkts 100 --output test/output/live_dac.bin
```

发端（8004，READY 后）：

```bash
cd ~/Fstack-DPDk/sender
sudo bash run_pktgen_sender.sh --to-8007 --count 100 --force-regen-pcap
```

**通过标准：** 收端日志 `matched 100/100`、`seq_gaps 0`、`Run status: COMPLETE`。

---

## Part 2 — 十万包压测（100000 包，1344 B，6 Gbps）

```bash
sudo bash run_pktgen_sender.sh --to-8007 --count 100000 --rate-mbps 6000 --force-regen-pcap
```

收端 `--expected-pkts 100000`。

---

## Part 3 — 线速峰值（200 万包）

```bash
# 线速
sudo bash run_pktgen_sender.sh --to-8007 --count 2000000 --rate 100 --force-regen-pcap

# 限速 1 Gbps，发 60 秒
sudo bash run_pktgen_sender.sh --to-8007 --duration 60 --rate-mbps 1000
```

---

## Part 4 — 反向 / 自环（→ 8004）

发端在 8007，收端在 8004：

```bash
# 收端在 8004 上启动（假设 8004 也部署了 Fstack-DPDk 收端）
sudo ./scripts/run_recv.sh --link dac --raw-payload --expected-pkts 1000 \
    --output test/output/live_to8004.bin

# 发端在 8007
cd ~/Fstack-DPDk/sender
sudo bash run_pktgen_sender.sh --to-8004 --count 1000 --force-regen-pcap
```

---

## 首次部署（发端，只做一次）

```bash
/usr/local/bin/dpdk-devbind.py -s   # 查 f1 PCI，勿用 SSH 口 0000:44:00.0

sudo bash run_pktgen_sender.sh --setup-dpdk --pci 0000:42:00.1
```

环境检查（不发包）：

```bash
sudo bash run_pktgen_sender.sh --check
```

---

## 速率参数

| 参数 | 含义 |
|------|------|
| `--rate PERCENT` | 线路速率百分比，`100` = 线速（10G 口约 10 Gbps wire） |
| `--rate-mbps MBPS` | 目标线路带宽 Mbps，自动换算百分比 |
| `--line-gbps N` | 网口标称线速（默认 `10`），供 `--rate-mbps` 换算 |

**换算示例（10G 口）：**

| 目标 | 命令 |
|------|------|
| 线速 ~10 Gbps | `--rate 100` 或 `--rate-mbps 10000` |
| ~6 Gbps | `--rate-mbps 6000`（≈60%） |
| ~1 Gbps | `--rate-mbps 1000`（≈10%） |

收端日志 `Throughput (burst, payload)` 为 **payload 口径**（1344B/包），比线路带宽低约 6%。

---

## 默认配置（`include/sender_config.h`）

| 宏 | 值 | 含义 |
|----|-----|------|
| `SEND_TARGET_8007_IP6` | `…:0002` | 8007 收端 IPv6 |
| `SEND_TARGET_8004_IP6` | `…:0003` | 8004 收端 IPv6（自环/反向） |
| `SEND_TARGET_8007_MAC` | `90:e2:ba:50:aa:71` | 8007 f1 MAC |
| `SEND_TARGET_8004_MAC` | `90:e2:ba:50:7c:f9` | 8004 f1 MAC |
| `SEND_UDP_DST_PORT` | `8888` | 收端监听端口 |
| `SEND_PAYLOAD_SIZE` | `1336` | 数据区字节；raw 下 UDP payload = 8B头 + 1336B体 = 1344B |
| `SEND_TX_PCI_DEFAULT` | `0000:42:00.1` | 发端 f1 PCI |

改 IP/MAC/端口后必须加 `--force-regen-pcap`。

### 命令行覆盖

| 参数 | 说明 |
|------|------|
| `--count N` | 包数（须与收端 `--expected-pkts` 一致） |
| `--payload-size B` | 数据区 1336（raw 下 UDP payload = 8B头 + 1336B体 = 1344B） |
| `--src-ip` / `--dst-ip` | IPv6，默认由 `--to-*` 决定 |
| `--src-mac` / `--dst-mac` | 默认由 `--to-*` 决定 |
| `--pci` | 发端 f1：`0000:42:00.1` |
| `--force-regen-pcap` | 改 count/IP/MAC 后必加 |
| `--no-release` | 打流结束后不自动还网卡/恢复 IPv6 |

---

## 程序A：单独生成 pcap（不发包）

```bash
# 生成 → 8007 的 1000 包 pcap（--payload-size 指数据区字节）
./build/gen_udp_pcap udp_test.pcap --to-8007 --count 1000 --payload-size 1336

# 生成 → 8004
./build/gen_udp_pcap udp_test.pcap --to-8004 --count 1000

# 自定义 IP/MAC
./build/gen_udp_pcap udp_test.pcap --to-8007 \
    --src-ip 1111:2222:1111:2222:1111:2222:1111:0003 \
    --dst-mac 90:e2:ba:50:aa:71 --count 1000
```
