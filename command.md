# 启动命令（Fstack-DPDk · 文档同步 2026-07-27）

**时序：收端先 READY → 发端再发包。** `--count` 须与收端 `--expected-pkts` 一致。

**当前默认拓扑：8007 发 → 8004 收**（8004 bind `…:0003`）。联调请显式加 `--raw-payload`（默认关）。


| 机器   | 工程路径                              | F-Stack                       |
| ---- | --------------------------------- | ----------------------------- |
| 8004 | `/home/cics/HMH/Fstack-DPDk_8004` | `/home/cics/HMH/F-stack`      |
| 8007 | `/home/dev/HMH--8007/Fstack-DPDk` | `/home/dev/HMH--8007/F-stack` |


---

## 构建（两端通用）

```bash
cd /home/cics/HMH/Fstack-DPDk_8004    # 8004
# 或 cd /home/dev/HMH--8007/Fstack-DPDk  # 8007

cmake -S . -B build -DFF_PATH=<本机 F-stack 路径>
cmake --build build -j"$(nproc)"
```

8004 收端示例：

```bash
sudo FF_PATH=/home/cics/HMH/F-stack ./scripts/run_recv.sh ...
```

---



## 场景 A — 8007 发 / 8004 收（当前默认）



### 8004 收端

```bash
cd /home/cics/HMH/Fstack-DPDk_8004
sudo FF_PATH=/home/cics/HMH/F-stack ./scripts/run_recv.sh \
    --link dac --raw-payload \
    --expected-pkts 100000 \
    --output test/output/live_to8004.bin
```

半旁路（冲高吞吐）：

```bash
sudo FF_PATH=/home/cics/HMH/F-stack ./scripts/run_recv.sh \
    --link dac --raw-payload --udp-bypass \
    --expected-pkts 1000000 \
    --output test/output/live_bypass.bin
```



### 8007 发端（SSH 非交互）

```bash
# 在 8004 上执行
sshpass -p 'ict123' ssh -o StrictHostKeyChecking=no -p 8007 dev@8.152.220.202 \
  'cd /home/dev/HMH--8007/Fstack-DPDk/sender && echo "ict123" | sudo -S env NO_PROMPT=1 \
   bash run_pktgen_sender.sh --to-8004 --count 2000000 --rate-mbps 6000 --force-regen-pcap'
```

或在 8007 本机：

```bash
cd /home/dev/HMH--8007/Fstack-DPDk/sender
sudo bash run_pktgen_sender.sh --to-8004 --count 2000000 --rate-mbps 6000 --force-regen-pcap
```

线速：`--rate 100`（或 `--rate-mbps 10000`）。

---



## 场景 B — 8004 发 / 8007 收



### 8007 收端

```bash
cd /home/dev/HMH--8007/Fstack-DPDk
sudo FF_PATH=/home/dev/HMH--8007/F-stack ./scripts/run_recv.sh --link dac --raw-payload \
    --expected-pkts 100000 \
    --output test/output/live_dac.bin
```

> 8007 收端 bind 为 `…:0002`。写盘路径若 peak 打满见 `docs/性能统计.md`「8007 作收端」。



### 8004 发端

```bash
cd /home/cics/HMH/Fstack-DPDk_8004/sender
sudo bash run_pktgen_sender.sh --to-8007 --count 100000 --rate-mbps 6000 --force-regen-pcap
```

---



## 冒烟测试（100 包）

**收端（8004）：**

```bash
sudo FF_PATH=/home/cics/HMH/F-stack ./scripts/run_recv.sh \
    --link dac --raw-payload --expected-pkts 100 \
    --output test/output/smoke.bin
```

**发端（8007）：**

```bash
cd /home/dev/HMH--8007/Fstack-DPDk/sender
sudo bash run_pktgen_sender.sh --to-8004 --count 100 --force-regen-pcap
```

**通过：** `matched 100/100`、`seq_gaps 0`、`Run status: COMPLETE`。

---



## 场景 C — TCP V2.0（内核发端）

`tx_csum_offoad_skip=1` 必开。发端：

```bash
cd sender
sudo bash run_tcp_sender.sh --to-8004 --count 100000 --rate-mbps 3000 --raw-payload
```

---



## 场景 D — TCP V2.1（双端 F-Stack）

```bash
# 8004 收端
sudo FF_PATH=/home/cics/HMH/F-stack ./scripts/run_recv.sh \
  --link dac --proto tcp --raw-payload --expected-pkts 1000000 \
  --output test/output/tcp_ff.bin

# 8007 发端
sshpass -p 'ict123' ssh -o StrictHostKeyChecking=no -p 8007 dev@8.152.220.202 \
  'cd /home/dev/HMH--8007/Fstack-DPDk && echo "ict123" | sudo -S env FF_PATH=/home/dev/HMH--8007/F-stack \
   bash scripts/run_tcp_fstack_send.sh --to-8004 --count 1000000 --raw-payload'
```

**注意：** 基线约 **~2.95 Gbps**；发端帧模板优化后单流约 **~3.6 Gbps**。勿覆盖 8007 的 `scripts/lib/link_profile.sh`。

---



## 场景 E — TCP 多核发端试打（traffic-only）

**这个就是懒得控制两台机器，直接在一个主机通过ssh控制另一台机器，交互跑得出结果，不过我尝试的不多稳定性不足，优先用上面的**



```bash
# 8004 收端
sudo FF_PATH=/home/cics/HMH/F-stack ./build/fstack_tcp_recv \
  -c configs/config.dac.tcpbench.ini -- \
  --output /dev/null --dst-port 8888 --raw-payload --payload-size 1344 \
  --expected-pkts 400000 --traffic-only --max-conns 4

# 8007 多进程发端
sshpass -p 'ict123' ssh -p 8007 dev@8.152.220.202 \
  'cd /home/dev/HMH--8007/Fstack-DPDk && echo ict123 | sudo -S env FF_PATH=/home/dev/HMH--8007/F-stack \
   bash scripts/run_tcp_fstack_send_mp.sh --to-8004 --workers 4 --count 400000 --raw-payload'
```

> F-Stack MP **TCP 客户端** 握手不稳定（见错误日志 §22）。

---



## 收端 `run_recv.sh` 参数

```bash
sudo ./scripts/run_recv.sh --link dac --raw-payload [options]
```


| 参数                  | 说明                              |
| ------------------- | ------------------------------- |
| `--link dac|fibre`  | dac=f1 万兆；fibre=f0 光口（仅 8007）   |
| `--proto udp|tcp`   | 默认 udp；tcp 用 `fstack_tcp_recv`  |
| `--raw-payload`     | 透传整段 payload（**需显式加**；默认解析 8B 头） |
| `--no-raw-payload`  | 解析 8B 头后落 data 区（脚本默认）         |
| `--expected-pkts N` | 收满 N 帧退出；`0`=持续至 Ctrl+C         |
| `--output PATH`     | 输出文件路径（默认 `test/output/live_<link>.bin`） |
| `--dst-port PORT`   | bind 端口（默认 **8888**）            |
| `--payload-size B`  | raw 模式帧长（默认 1344）               |
| `--udp-bypass`      | UDP 半旁路（线速约 ~9.4G）              |
| `--zc-recv`         | `ff_zc_recv`（几乎无增益；与 bypass 互斥） |
| `--kni-ops`         | 配置 veth0 运维 IPv6 `…:0004`       |
| `--no-restore`      | 退出不 release                     |
| `-- …`              | 透传给二进制（如 `--traffic-only --max-conns N`） |


退出时默认自动 `release_env.sh`。

---



## 发端 `run_pktgen_sender.sh` 参数（UDP）

```bash
cd sender
sudo bash run_pktgen_sender.sh [options]
```


| 参数                                                  | 说明                              |
| --------------------------------------------------- | ------------------------------- |
| `--to-8007`                                         | 目标 8007（`…:0002` / MAC `aa:71`） |
| `--to-8004`                                         | 目标 8004（`…:0003` / MAC `7c:f9`） |
| `--count N`                                         | 发包数（与收端一致）                      |
| `--rate-mbps MBPS`                                  | 线路带宽限速                          |
| `--rate PERCENT`                                    | 线路速率百分比（`100`=线速）               |
| `--line-gbps N`                                     | 网口标称线速（默认 10），供换算               |
| `--force-regen-pcap`                                | 强制重建 pcap                       |
| `--setup-dpdk`                                      | 首次部署：绑驱动 + hugepages            |
| `--check`                                           | 环境检查                            |
| `--no-release`                                      | 结束后不自动还网卡                       |
| `--src-ip` / `--dst-ip` / `--src-mac` / `--dst-mac` | 覆盖默认                            |
| `--pci`                                             | 发端口 PCI（默认 `0000:42:00.1`）      |
| `NO_PROMPT=1`                                       | 跳过 Enter 确认（SSH 自动化）            |


**10G 口换算：** `--rate-mbps 6000` ≈ 60% ≈ 6 Gbps；`--rate 100` = 线速。  
收端 `Throughput (burst, payload)` 通常略低于线路 Mbps（帧头开销）。

---



## 发端 `run_tcp_sender.sh` 参数（TCP）

```bash
cd sender
sudo bash run_tcp_sender.sh --to-8004 --count 100000 --rate-mbps 3000 --raw-payload
```


| 参数                        | 说明                    |
| ------------------------- | --------------------- |
| `--to-8004` / `--to-8007` | 目标                    |
| `--count N`               | 帧数                    |
| `--rate-mbps M`           | 目标 payload 比特率；省略则尽力发 |
| `--payload-size B`        | 数据区字节（帧长=8+B，默认 1336） |
| `--raw-payload`           | 与收端 raw 成帧对齐（默认）      |


---



## fibre 收端（8007，交换板发流）

```bash
# 8007
sudo FF_PATH=/home/dev/HMH--8007/F-stack ./scripts/run_recv.sh --link fibre --raw-payload \
    --payload-size 1344 --output test/output/live_fibre.bin
```

发端为交换板 FPGA（`…:0001`），不使用 sender。

---



## 首次部署

**收端：** `run_recv.sh` 首次运行自动 `setup_env.sh`。

**发端：**

```bash
cd sender
# 用 dpdk-devbind.py -s 确认 PCI（勿绑 SSH 管理口）
sudo bash run_pktgen_sender.sh --setup-dpdk --pci 0000:42:00.1
sudo bash run_pktgen_sender.sh --check
```

TCP 发端不绑 DPDK，只需 f1 有 IPv6。

---



## 落盘校验

本仓暂无独立落盘校验脚本。raw 可用文件大小粗验：

```bash
# raw 1344B × N 包 → 期望约 N * 1344
stat -c '%s' test/output/live_to8004.bin
```

pcap 生成逻辑自环：`bash test/self_loop_pcap.sh`。

---



## 程序 A：单独生成 pcap

```bash
./build/sender/gen_udp_pcap udp_test.pcap --to-8004 --count 5000 --payload-size 1336
```

---



## 注意点

1. **同一网卡互斥**：收端与发端不能同时占用 f1；切换角色等上一进程退出。
2. **收端 bind IP**：8004=`…:0003`，8007=`…:0002`；改后须重编。
3. **发端自动 release**：正常退出 / Ctrl+C 还回 `ixgbe`；连续测试加 `--no-release`。
4. **pktgen 大数量**：模板 5000 包循环；发端自动加余量避尾截断（收端仍用原始 `--count`）。
5. **SSH 发端（8004→8007）：** `dev@8.152.220.202:8007`，密码 `ict123`。
6. **TCP TX csum：** 须 `tx_csum_offoad_skip=1`。
7. **fibre 仅 8007 收**；dac 才做角色互换。
8. **杀收端进程：** 用 `pgrep -x fstack_udp_recv`，避免 `pkill -f run_recv` 误杀 shell。

