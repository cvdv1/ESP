#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
分析 Logic 2 导出的 digital.csv，逐通道给出频率/占空比/抖动/脉宽/协议解码结果。

用法：
    python tools/analyze_capture.py digital.csv [--sample-rate 12000000]

CSV 格式（Logic 2 "Export Data → CSV"）：第一列 Time [s]，后面 8 列 Channel 0..7。
导出行通常是"某个通道有变化才写一行"，所以本脚本直接把这些跳变当作边沿使用（比逐点扫描更准）。

只用标准库，不需要 pandas。
"""
import argparse
import csv
import statistics
import sys


def load(path):
    times, chans = [], [[] for _ in range(8)]
    with open(path, newline="", encoding="utf-8-sig") as f:
        r = csv.reader(f)
        header = next(r)
        ncol = sum(1 for h in header if h.lower().startswith("channel"))
        ncol = max(ncol, 8)
        for row in r:
            if len(row) < ncol + 1:
                continue
            times.append(float(row[0]))
            for i in range(8):
                chans[i].append(int(row[i + 1]))
    return header, times, chans


def describe(xs, name, unit=1e6, fmt="{:.4f}"):
    if not xs:
        return
    s = sorted(xs)
    med = statistics.median(xs)
    sd = statistics.pstdev(xs) if len(xs) > 1 else 0.0
    print(f"  {name}: 中位 {fmt.format(med*unit)}, 均值 {fmt.format(statistics.mean(xs)*unit)}, "
          f"sd {fmt.format(sd*unit)}, 5%~95% {fmt.format(s[int(0.05*(len(s)-1))]*unit)}"
          f"~{fmt.format(s[int(0.95*(len(s)-1))]*unit)}, "
          f"min {fmt.format(s[0]*unit)}, max {fmt.format(s[-1]*unit)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--sample-rate", type=float, default=12e6)
    a = ap.parse_args()

    header, times, chans = load(a.csv)
    n = len(times)
    if n < 100:
        sys.exit("数据太少，检查 CSV 是否正确")
    span = times[-1] - times[0]
    print(f"文件: {a.csv}")
    print(f"列: {', '.join(header[:9])}")
    print(f"行数 {n:,} | 时长 {span*1000:.2f} ms | 采样率 {a.sample_rate/1e6:g} MS/s "
          f"(1 个采样点 = {1e9/a.sample_rate:.1f} ns)\n")

    def level(i, t):
        lo, hi = 0, n - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if times[mid] <= t:
                lo = mid
            else:
                hi = mid - 1
        return chans[i][lo]

    edges = []
    for i in range(8):
        rise, fall = [], []
        for k in range(1, n):
            if chans[i][k] != chans[i][k - 1]:
                (rise if chans[i][k] else fall).append(times[k])
        edges.append((rise, fall))
        print(f"D{i}: 上升沿 {len(rise):6d} | 下降沿 {len(fall):6d}")

    # ---- 周期与抖动 ----
    print("\n=== 连续波形：周期 / 抖动 / 占空比 ===")
    for i in range(8):
        rise, fall = edges[i]
        if len(rise) < 4:
            continue
        per = [rise[k + 1] - rise[k] for k in range(len(rise) - 1)][1:-1]
        if not per:
            continue
        med = statistics.median(per)
        if max(per) / med > 10:
            print(f"D{i}: 非连续信号（突发/协议/长空闲），跳过周期统计")
            continue
        print(f"D{i}: 频率 {1/med:.4f} Hz", end="")
        # 占空比（只在连续周期内统计）
        duty = []
        j = 0
        for k in range(len(rise) - 1):
            rt = rise[k]
            while j < len(fall) and fall[j] <= rt:
                j += 1
            if j < len(fall) and fall[j] < rise[k + 1]:
                per_k = rise[k + 1] - rt
                if abs(per_k - med) / med < 0.05:
                    duty.append((fall[j] - rt) / per_k)
        if duty:
            print(f"，占空比 中位 {statistics.median(duty)*100:.3f}%"
                  f"（min {min(duty)*100:.2f}% ~ max {max(duty)*100:.2f}%）", end="")
        print()
        describe(per, f"D{i} 周期", 1e6)

    # ---- D2 RMT 突发 ----
    rise2, fall2 = edges[2]
    rise7 = edges[7][0]
    if rise2:
        bursts, cur = [], [rise2[0]]
        for t in rise2[1:]:
            if t - cur[-1] > 200e-6:
                bursts.append(cur)
                cur = [t]
            else:
                cur.append(t)
        bursts.append(cur)
        widths = []
        j = 0
        for rt in rise2:
            while j < len(fall2) and fall2[j] <= rt:
                j += 1
            if j < len(fall2):
                widths.append((fall2[j] - rt) * 1e6)
        print(f"\n=== D2 RMT 突发 ===")
        print(f"  簇数 {len(bursts)}，每簇脉冲数 {sorted({len(b) for b in bursts})}")
        print(f"  脉宽范围 {min(widths):.2f} ~ {max(widths):.2f} µs，首簇: "
              + ", ".join(f"{w:.2f}" for w in widths[:16]))
        if len(bursts) > 1:
            starts = [b[0] for b in bursts]
            describe([starts[k+1]-starts[k] for k in range(len(starts)-1)], "簇周期", 1e3)
        if rise7:
            offs = []
            for t7 in rise7:
                cand = [t for t in rise2 if t > t7]
                if cand and cand[0] - t7 < 5e-3:
                    offs.append(cand[0] - t7)
            describe(offs, "SYNC→首脉冲", 1e6)

    # ---- D7 SYNC ----
    rise7, fall7 = edges[7]
    if len(rise7) > 1:
        print("\n=== D7 SYNC ===")
        describe([rise7[k+1]-rise7[k] for k in range(len(rise7)-1)], "周期", 1e3, "{:.3f}")
        w = []
        for rt in rise7:
            cand = [f for f in fall7 if f > rt]
            if cand:
                w.append(cand[0] - rt)
        describe(w, "高电平宽度", 1e6, "{:.3f}")

    # ---- D5 / D6 SPI ----
    rise5 = edges[5][0]
    if len(rise5) > 2:
        clusters, cur = [], [rise5[0]]
        for t in rise5[1:]:
            if t - cur[-1] > 5e-6:
                clusters.append(cur)
                cur = [t]
            else:
                cur.append(t)
        clusters.append(cur)
        print("\n=== D5 SCLK / D6 MOSI（SPI mode0, MSB first）===")
        print(f"  时钟簇 {len(clusters)} 个，每簇时钟数 {[len(c) for c in clusters]}")
        if len(clusters[0]) > 1:
            describe([clusters[0][k+1]-clusters[0][k] for k in range(len(clusters[0])-1)],
                     "时钟周期", 1e9, "{:.1f}")
        ok = 0
        for c in clusters:
            bits = "".join(str(level(6, t)) for t in c)
            if len(bits) >= 32:
                vals = [int(bits[i:i+8], 2) for i in range(0, 32, 8)]
                s = " ".join(f"{v:02X}" for v in vals)
                if s == "AA 55 F0 0F":
                    ok += 1
                else:
                    print(f"  异常帧: {s}")
        print(f"  解出 AA 55 F0 0F 的帧: {ok}/{len(clusters)}")

    # ---- D4 UART ----
    rise4, fall4 = edges[4]
    if fall4:
        bit = 1 / 115200.0
        starts = []
        for t in fall4:
            prev = [r for r in rise4 if r < t]
            if prev and t - prev[-1] > 10 * bit:
                starts.append(t)
        texts = []
        for t0 in starts:
            chars, tc = [], t0
            for _ in range(16):
                if level(4, tc + 0.5 * bit) != 0 or level(4, tc + 9.5 * bit) != 1:
                    break
                b = 0
                for k in range(8):
                    b |= level(4, tc + (k + 1.5) * bit) << k
                chars.append(b)
                tc += 10 * bit
            texts.append((len(chars),
                          "".join(chr(b) if 32 <= b < 127 else {13: "\\r", 10: "\\n"}.get(b, "?")
                                  for b in chars)))
        print("\n=== D4 UART（115200 8N1）===")
        print(f"  帧数 {len(texts)}，每帧字符数 {sorted({c for c, _ in texts})}")
        if texts:
            print(f"  解码文本: {texts[0][1]!r}")
            print(f"  全部帧一致: {len({t for _, t in texts}) == 1}")
        all4 = sorted(rise4 + fall4)
        d = [all4[i+1]-all4[i] for i in range(len(all4)-1) if 0 < all4[i+1]-all4[i] < 30e-6]
        if d:
            describe(d, "相邻跳变间隔（≈1 bit）", 1e6, "{:.3f}")


if __name__ == "__main__":
    main()
