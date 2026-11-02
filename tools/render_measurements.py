#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 Logic 2 导出的 digital.csv 渲染成实测波形图（docs/measurements/*.png）。

用法（在仓库根目录）：
    python tools/render_measurements.py
    python tools/render_measurements.py --csv docs/measurements/digital.csv --out docs/measurements

依赖：numpy、matplotlib（Windows 上会自动使用微软雅黑显示中文）。
"""
import argparse
import csv
import os
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager

for _f in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\simhei.ttf"):
    try:
        font_manager.fontManager.addfont(_f)
    except Exception:
        pass
plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(path):
    t, ch = [], [[] for _ in range(8)]
    with open(path, newline="", encoding="utf-8-sig") as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            if len(row) < 9:
                continue
            t.append(float(row[0]))
            for i in range(8):
                ch[i].append(int(row[i + 1]))
    return np.array(t), [np.array(c) for c in ch]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default=os.path.join(ROOT, "docs", "measurements", "digital.csv"))
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "measurements"))
    a = ap.parse_args()
    if not os.path.exists(a.csv):
        sys.exit(f"找不到 {a.csv}")
    os.makedirs(a.out, exist_ok=True)

    t, ch = load(a.csv)

    def step(i):
        return (np.concatenate([t, [t[-1] + 1e-7]]),
                np.concatenate([ch[i], [ch[i][-1]]]))

    def rises(i):
        return t[1:][np.where((ch[i][1:] == 1) & (ch[i][:-1] == 0))[0]]

    def falls(i):
        return t[1:][np.where((ch[i][1:] == 0) & (ch[i][:-1] == 1))[0]]

    names = ["D0 1 kHz 50% 方波", "D1 10 kHz PWM 占空比扫描", "D2 RMT 任意脉宽突发",
             "D3 软件 bit-bang（抖动）", "D4 UART1 TX 115200", "D5 SPI SCLK 1 MHz",
             "D6 SPI MOSI", "D7 SYNC 触发"]

    # ---------- 图 1：100 ms 全览 ----------
    t0, win = 0.0210, 0.100
    fig, axes = plt.subplots(8, 1, figsize=(13, 9), sharex=True, gridspec_kw={"hspace": 0.30})
    for i, ax in enumerate(axes):
        tt, yy = step(i)
        ax.step(tt, yy, where="post", lw=0.9, color=f"C{i}")
        ax.set_ylim(-0.3, 1.3)
        ax.set_yticks([])
        ax.set_ylabel(names[i], rotation=0, ha="right", va="center", fontsize=9)
        ax.grid(True, axis="x", ls=":", alpha=0.35)
        for s in ("top", "right", "left"):
            ax.spines[s].set_visible(False)
    axes[-1].set_xlim(t0, t0 + win)
    axes[-1].set_xlabel("时间 (s)")
    fig.suptitle("ESP32-S3 八路波形实测：12 MS/s，100 ms 窗口 = 2 个 50 ms 时基周期", fontsize=13, y=0.975)
    fig.text(0.5, 0.935,
             "D1（10 kHz）与 D3（342.9 kHz）在该缩放下为实心色带；D2 簇 / D5 时钟 / D6 数据在此尺度上是一根细线，细节见 waveform_zoom.png",
             ha="center", fontsize=9, color="0.35")
    fig.subplots_adjust(left=0.21, right=0.98, top=0.90, bottom=0.06)
    fig.savefig(os.path.join(a.out, "waveform_overview.png"), dpi=130)
    plt.close(fig)

    # ---------- 图 2：细节 ----------
    fig = plt.figure(figsize=(13, 11))
    gs = fig.add_gridspec(4, 1, height_ratios=[1, 1.15, 1, 1.15], hspace=0.62)

    # ① RMT
    ax = fig.add_subplot(gs[0])
    s0 = [x for x in rises(7) if x > 0.02][0]
    tt, y7 = step(7)
    t2, y2 = step(2)
    ax.step(tt[(tt > s0 - 5e-5) & (tt < s0 + 9e-4)], y7[(tt > s0 - 5e-5) & (tt < s0 + 9e-4)] + 1.35,
            where="post", lw=1.3, color="C7")
    ax.step(t2[(t2 > s0 - 5e-5) & (t2 < s0 + 9e-4)], y2[(t2 > s0 - 5e-5) & (t2 < s0 + 9e-4)],
            where="post", lw=1.3, color="C2")
    ax.set_xlim(s0 - 5e-5, s0 + 9e-4)
    ax.set_ylim(-0.3, 2.5)
    ax.set_yticks([0.5, 1.85])
    ax.set_yticklabels(["D2 RMT", "D7 SYNC"])
    ax.set_title("细节①  SYNC 后 529.75 µs 出现 RMT 簇：16 个脉冲，高电平 2→17 µs（步进 1 µs），间隔 2 µs，簇长 182 µs",
                 fontsize=11)
    ax.grid(True, ls=":", alpha=0.35)
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.annotate("", xy=(s0, 1.9), xytext=(s0 + 529.75e-6, 1.9),
                arrowprops=dict(arrowstyle="<->", lw=1.0, color="0.35"))
    ax.text(s0 + 0.00026, 2.05, "529.75 µs", ha="center", fontsize=9, color="0.3")

    # ② SPI
    ax = fig.add_subplot(gs[1])
    c0 = [x for x in rises(5) if x > 0.02][0]
    w = 34e-6
    t5, y5 = step(5)
    t6, y6 = step(6)
    ax.step(t5[(t5 > c0 - 2e-6) & (t5 < c0 + w + 2e-6)], y5[(t5 > c0 - 2e-6) & (t5 < c0 + w + 2e-6)] + 1.35,
            where="post", lw=1.3, color="C5")
    ax.step(t6[(t6 > c0 - 2e-6) & (t6 < c0 + w + 2e-6)], y6[(t6 > c0 - 2e-6) & (t6 < c0 + w + 2e-6)],
            where="post", lw=1.3, color="C6")
    ax.set_xlim(c0 - 2e-6, c0 + w + 2e-6)
    ax.set_ylim(-0.9, 2.5)
    ax.set_yticks([0.5, 1.85])
    ax.set_yticklabels(["D6 MOSI", "D5 SCLK"])
    ax.set_title("细节②  SPI 一帧：32 个 1.000 MHz 时钟，MOSI = AA 55 F0 0F（MSB first，mode 0，上升沿采样）", fontsize=11)
    for k, b in enumerate(["AA", "55", "F0", "0F"]):
        x = c0 + k * 8e-6
        ax.axvline(x, color="0.8", lw=0.8, ls="--")
        ax.text(x + 4e-6, -0.55, b, ha="center", fontsize=10, family="monospace")
    ax.grid(True, axis="x", ls=":", alpha=0.35)
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)

    # ③ UART
    ax = fig.add_subplot(gs[2])
    bit = 1 / 115200
    starts = []
    for x in falls(4):
        prev = rises(4)[rises(4) < x]
        if len(prev) and x - prev[-1] > 10 * bit:
            starts.append(x)
    u0 = starts[0]
    t4, y4 = step(4)
    w4 = 3 * 10 * bit
    sel4 = (t4 > u0 - 2e-6) & (t4 < u0 + w4 + 2e-6)
    ax.step(t4[sel4], y4[sel4], where="post", lw=1.5, color="C4")
    ax.set_xlim(u0 - 2e-6, u0 + w4 + 2e-6)
    ax.set_ylim(-0.5, 1.6)
    ax.set_yticks([0, 1])
    ax.set_yticklabels(["0", "1"])
    ax.set_title("细节③  UART 115200 8N1（位宽 8.667 µs）：每 50 ms 一帧 13 字符 \"ESP32-S3 LA\\r\\n\"", fontsize=11)
    for k, c in enumerate("ESP"):
        x = u0 + k * 10 * bit
        ax.axvline(x, color="0.8", lw=0.8, ls="--")
        ax.text(x + 5 * bit, -0.32, c, ha="center", fontsize=11, family="monospace")
    ax.grid(True, axis="x", ls=":", alpha=0.35)
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)

    # ④ 抖动对比
    ax = fig.add_subplot(gs[3])

    def per(i):
        return np.diff(rises(i))[1:-1]

    p0, p3 = per(0) * 1e9, per(3) * 1e9
    vals = {"D0 硬件 PWM": (p0.std(), p0.max() - p0.min()),
            "D3 软件 bit-bang": (p3.std(), p3.max() - p3.min())}
    x = np.arange(2)
    width = 0.34
    ax.bar(x - width / 2, [v[0] for v in vals.values()], width, label="周期标准差 (ns)", color="C0")
    ax.bar(x + width / 2, [v[1] for v in vals.values()], width, label="峰峰抖动 (ns)", color="C1")
    ax.set_yscale("log")
    ax.set_xticks(x)
    ax.set_xticklabels(list(vals.keys()))
    ax.set_ylabel("ns（对数）")
    ax.set_title("细节④  周期抖动：同一个工程、同一块板，硬件时序 vs 软件时序", fontsize=11)
    for k, (sd, pp) in enumerate(vals.values()):
        ax.text(k - width / 2, sd * 1.25, f"{sd:.0f}", ha="center", fontsize=9)
        ax.text(k + width / 2, pp * 1.25, f"{pp:.0f}", ha="center", fontsize=9)
    ax.legend(fontsize=9, loc="upper left")
    ax.grid(True, axis="y", ls=":", alpha=0.35)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)

    fig.suptitle("细节波形与抖动统计（数据来自 docs/measurements/digital.csv）", fontsize=12, y=0.985)
    fig.savefig(os.path.join(a.out, "waveform_zoom.png"), dpi=130)
    plt.close(fig)
    print("已生成:", os.path.join(a.out, "waveform_overview.png"))
    print("已生成:", os.path.join(a.out, "waveform_zoom.png"))


if __name__ == "__main__":
    main()
