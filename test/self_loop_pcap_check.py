#!/usr/bin/env python3
"""
test/self_loop_pcap_check.py

pcap 自环校验器：读取 gen_udp_pcap 生成的 IPv6 UDP pcap，按收端 udp_payload.h
的解析逻辑回读校验。

校验项（与 include/udp_payload.h 一致）：
  - pcap 文件头 magic / linktype=1
  - 每包 EtherType 0x86dd、IPv6 version=6 nexthdr=17(UDP) hlim=64
  - UDP dst_port=8888、UDP length 自洽
  - UDP checksum 按 IPv6 pseudo header 重算并比对（IPv6 必须非零）
  - payload 8B 头：seq(host order) 递增 0,1,2,...；declared_len == payload_size
  - data 区递增字节：data[j] == (i * payload_size + j) & 0xFF（跨包连续）
  - 包数 == count

用法: python3 self_loop_pcap_check.py <pcap> <count> <payload_size> <target>
退出码: 0=通过, 非0=失败
"""
import struct
import sys
import socket


def ones_comp(buf: bytes) -> int:
    s = 0
    for i in range(0, len(buf) - 1, 2):
        s += (buf[i] << 8) | buf[i + 1]
    if len(buf) % 2:
        s += buf[-1] << 8
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return (~s) & 0xFFFF


def udp6_checksum(src6: bytes, dst6: bytes, udp_seg: bytes) -> int:
    ulen_be = struct.pack(">I", len(udp_seg))
    ph = src6 + dst6 + ulen_be + b"\x00\x00\x00\x11"  # zero(3)+nexthdr(17)
    total = ph + udp_seg
    if len(total) % 2:
        total += b"\x00"
    cs = ones_comp(total)
    return cs if cs != 0 else 0xFFFF


def main() -> int:
    if len(sys.argv) != 5:
        print("用法: self_loop_pcap_check.py <pcap> <count> <payload_size> <target>", file=sys.stderr)
        return 2
    pcap = sys.argv[1]
    count = int(sys.argv[2])
    payload_size = int(sys.argv[3])
    target = sys.argv[4]

    expect_dst_ip = {
        "8007": "1111:2222:1111:2222:1111:2222:1111:0002",
        "8004": "1111:2222:1111:2222:1111:2222:1111:0003",
    }[target]
    expect_dst_ip6 = socket.inet_pton(socket.AF_INET6, expect_dst_ip)
    expect_dst_mac = {
        "8007": "90:e2:ba:50:aa:71",
        "8004": "90:e2:ba:50:7c:f9",
    }[target]

    errors = []
    n_pkts = 0

    with open(pcap, "rb") as f:
        # pcap 文件头 24B
        fh = f.read(24)
        magic = fh[:4]
        if magic not in (b"\xa1\xb2\xc3\xd4", b"\xd4\xc3\xb2\xa1"):
            errors.append(f"pcap magic 非法: {magic.hex()}")
            return report(pcap, target, count, n_pkts, errors)
        linktype = struct.unpack("<I", fh[20:24])[0]
        if linktype != 1:
            errors.append(f"linktype={linktype} (期望 1 Ethernet)")

        seq_expected = 0
        for i in range(count):
            ph = f.read(16)
            if len(ph) < 16:
                errors.append(f"包 {i}: pcap 记录头不完整（已读 {len(ph)} B）")
                break
            incl = struct.unpack("<I", ph[8:12])[0]
            frame = f.read(incl)
            if len(frame) < incl:
                errors.append(f"包 {i}: 帧数据不完整")
                break
            n_pkts += 1

            off = 0
            dst_mac = frame[off:off + 6]; off += 6
            src_mac = frame[off:off + 6]; off += 6
            ethertype = struct.unpack(">H", frame[off:off + 2])[0]; off += 2

            mac_str = ":".join(f"{b:02x}" for b in dst_mac)
            if mac_str != expect_dst_mac:
                errors.append(f"包 {i}: dst_mac={mac_str} != {expect_dst_mac}")
            if ethertype != 0x86DD:
                errors.append(f"包 {i}: ethertype=0x{ethertype:04x} != 0x86dd")

            # IPv6 头 40B
            if off + 40 > len(frame):
                errors.append(f"包 {i}: IPv6 头不完整")
                continue
            vtf = struct.unpack(">I", frame[off:off + 4])[0]; off += 4
            plen = struct.unpack(">H", frame[off:off + 2])[0]; off += 2
            nexthdr = frame[off]; off += 1
            hlim = frame[off]; off += 1
            src6 = frame[off:off + 16]; off += 16
            dst6 = frame[off:off + 16]; off += 16

            if (vtf >> 28) != 6:
                errors.append(f"包 {i}: IPv6 version={vtf >> 28} != 6")
            if nexthdr != 17:
                errors.append(f"包 {i}: nexthdr={nexthdr} != 17(UDP)")
            if hlim != 64:
                errors.append(f"包 {i}: hlim={hlim} != 64 (非错误，仅提示)")
            dst_ip_str = socket.inet_ntop(socket.AF_INET6, dst6)
            if dst6 != expect_dst_ip6:
                errors.append(f"包 {i}: dst6={dst_ip_str} != {expect_dst_ip}")

            # UDP 段 = plen 字节
            if off + plen > len(frame):
                errors.append(f"包 {i}: UDP 段长度 {plen} 超出帧剩余")
                continue
            udp_seg = frame[off:off + plen]
            sp, dp, ulen, cs = struct.unpack(">HHHH", udp_seg[:8])
            if dp != 8888:
                errors.append(f"包 {i}: dst_port={dp} != 8888")
            if ulen != plen:
                errors.append(f"包 {i}: UDP length={ulen} != IPv6 payload_len={plen}")

            # UDP checksum 重算：先把 chksum 字段置 0（标准做法），再按 IPv6 pseudo header 算
            udp_seg_zero = udp_seg[:6] + b"\x00\x00" + udp_seg[8:]
            recompute = udp6_checksum(src6, dst6, udp_seg_zero)
            if cs == 0:
                errors.append(f"包 {i}: UDP checksum=0（IPv6 不允许）")
            elif cs != recompute:
                errors.append(f"包 {i}: UDP checksum=0x{cs:04x} != 重算 0x{recompute:04x}")

            # payload = UDP header(8) 之后
            payload = udp_seg[8:]
            if len(payload) < 8:
                errors.append(f"包 {i}: payload 过短 ({len(payload)} B)")
                continue
            seq, declared = struct.unpack("<II", payload[:8])
            data = payload[8:]
            if seq != seq_expected:
                errors.append(f"包 {i}: seq={seq} != 期望 {seq_expected}")
            if declared != payload_size:
                errors.append(f"包 {i}: declared_len={declared} != {payload_size}")
            if len(data) != payload_size:
                errors.append(f"包 {i}: data 长度 {len(data)} != {payload_size}")
            else:
                # 递增字节 (i * payload_size + j) & 0xFF
                bad = 0
                base = i * payload_size
                for j in range(payload_size):
                    if data[j] != (base + j) & 0xFF:
                        bad += 1
                        if bad <= 3:
                            errors.append(
                                f"包 {i} data[{j}]={data[j]} != {(base + j) & 0xFF:#x}")
                if bad > 3:
                    errors.append(f"包 {i}: data 递增模式不符共 {bad} 字节")
            seq_expected += 1

    if n_pkts != count:
        errors.append(f"包数 {n_pkts} != 期望 {count}")

    return report(pcap, target, count, n_pkts, errors)


def report(pcap, target, count, n_pkts, errors) -> int:
    print(f"  pcap        : {pcap}")
    print(f"  target      : --to-{target}")
    print(f"  期望包数    : {count}")
    print(f"  实际包数    : {n_pkts}")
    if errors:
        nerr = len(errors)
        print(f"  错误数      : {nerr}")
        for e in errors[:20]:
            print(f"    [FAIL] {e}")
        if len(errors) > 20:
            print(f"    ... 还有 {len(errors) - 20} 条错误")
        print("  结果        : FAIL")
        return 1
    print("  结果        : PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
