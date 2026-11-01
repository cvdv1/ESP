#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
按 main/wave_pins.h 里的参数，推算出"逻辑分析仪上应该量到多少"。

用途：实测值和这里的数字对不上时，就说明固件参数被改过、或测量设置有问题
（采样率太低、探针接错脚、没共地……）。改 wave_pins.h 之后请同步改本文件。

运行：python tools/expected_values.py
"""

APB_HZ = 80_000_000  # ESP32-S3 低速模式 LEDC 的默认时钟源（APB）

# ---- 与 wave_pins.h 保持一致 ----
SQUARE_FREQ_HZ = 1000
SQUARE_DUTY_BITS = 10

PWM_FREQ_HZ = 10_000
PWM_DUTY_BITS = 8
PWM_DUTY_MIN_PCT = 10
PWM_DUTY_MAX_PCT = 90
PWM_SWEEP_STEP_MS = 10
PWM_SWEEP_STEPS = 100

RMT_RESOLUTION_HZ = 1_000_000
RMT_PULSE_NUM = 16
RMT_FIRST_PULSE_US = 2
RMT_STEP_US = 1
RMT_GAP_US = 2
RMT_TAIL_US = 10

UART_BAUD = 115200
UART_MSG = "ESP32-S3 LA\r\n"

SPI_CLOCK_HZ = 1_000_000
SPI_BYTES = (0xAA, 0x55, 0xF0, 0x0F)

BURST_PERIOD_MS = 50
SYNC_PULSE_US = 10
BURST_SYNC_TO_RMT_US = 500
BURST_RMT_TO_UART_US = 1300
BURST_UART_TO_SPI_US = 2000

BITBANG_HALF_US = 1


def ledc_divider(freq_hz, bits):
    """LEDC 分频系数：freq = APB / (divider * 2^bits)"""
    return APB_HZ / (freq_hz * (1 << bits))


def pct_to_duty(pct, max_count):
    """四舍五入，和 C 里的 (pct * max_count + 50) / 100 完全一致"""
    return (pct * max_count + 50) // 100


def main():
    print("=" * 78)
    print("CH0  LEDC 方波")
    print("=" * 78)
    full0 = 1 << SQUARE_DUTY_BITS          # 计数值满量程 = 2^bits
    duty0 = ((full0 - 1) // 2) + 1         # 和 C 里 max_count/2+1 一致（max_count = 1023）
    print(f"  频率        : {SQUARE_FREQ_HZ} Hz  (周期 {1e6 / SQUARE_FREQ_HZ:.1f} us)")
    print(f"  占空比      : {duty0}/{full0} = {100 * duty0 / full0:.2f}% "
          f"→ 高电平 {1e6 / SQUARE_FREQ_HZ * duty0 / full0:.1f} us")
    print(f"  分辨率      : {SQUARE_DUTY_BITS} bit（步进 {1e6 / SQUARE_FREQ_HZ / full0:.3f} us/级）")
    print(f"  分频系数    : {ledc_divider(SQUARE_FREQ_HZ, SQUARE_DUTY_BITS):.3f}"
          "（LEDC 分频有 8 bit 小数位，78.125 可精确表示 → 误差 0%）")
    print(f"  该分辨率下的频率上限: {APB_HZ / full0 / 1e3:.1f} kHz")

    print()
    print("=" * 78)
    print("CH1  LEDC 占空比三角扫描")
    print("=" * 78)
    full1 = 1 << PWM_DUTY_BITS             # 256
    max1 = full1 - 1                       # 255，pct_to_duty 的取值上限
    dmin = pct_to_duty(PWM_DUTY_MIN_PCT, max1)
    dmax = pct_to_duty(PWM_DUTY_MAX_PCT, max1)
    period_us = 1e6 / PWM_FREQ_HZ
    print(f"  载波频率    : {PWM_FREQ_HZ} Hz  (周期 {period_us:.1f} us)")
    print(f"  占空比范围  : 计数值 {dmin}~{dmax} / {full1} = "
          f"{100 * dmin / full1:.2f}% ~ {100 * dmax / full1:.2f}%")
    print(f"  对应脉宽    : {period_us * dmin / full1:.2f} us ~ {period_us * dmax / full1:.2f} us")
    print(f"  扫描周期    : {PWM_SWEEP_STEPS * PWM_SWEEP_STEP_MS} ms "
          f"（{PWM_SWEEP_STEPS} 步 × {PWM_SWEEP_STEP_MS} ms）")
    print(f"  分频系数    : {ledc_divider(PWM_FREQ_HZ, PWM_DUTY_BITS):.3f}（可精确表示）")
    print(f"  该分辨率下的频率上限: {APB_HZ / full1 / 1e3:.1f} kHz")
    print("  注意        : 8 bit 时 100% 占空比不可达（最大 255/256 = 99.6%）")

    print()
    print("=" * 78)
    print("CH2  RMT 任意脉宽突发")
    print("=" * 78)
    total = 0
    for i in range(RMT_PULSE_NUM):
        high = RMT_FIRST_PULSE_US + i * RMT_STEP_US
        total += high + RMT_GAP_US
        if i < 4 or i >= RMT_PULSE_NUM - 3:
            print(f"  脉冲 {i:2d}    : 高 {high:2d} us + 低 {RMT_GAP_US} us")
        elif i == 4:
            print("  ...")
    total += RMT_TAIL_US
    print(f"  一簇总长    : {total} us（每个 tick = {1e6 / RMT_RESOLUTION_HZ:.3f} us）")
    print(f"  最短脉冲    : {RMT_FIRST_PULSE_US} us —— 采样率至少 "
          f"{4_000_000 / 1_000_000:.0f} MS/s 才有 8 个点，建议 12 MS/s（24 个点）")
    print(f"  重复周期    : {BURST_PERIOD_MS} ms（相对 SYNC 上升沿 +{BURST_SYNC_TO_RMT_US} us）")

    print()
    print("=" * 78)
    print("CH3  软件 bit-bang")
    print("=" * 78)
    print(f"  设计值      : 半周期 {BITBANG_HALF_US} us → 理论上 {1e6 / (2 * BITBANG_HALF_US) / 1000:.0f} kHz")
    print("  实测会偏低（每次函数调用有几十到几百 ns 开销），且频率/占空比都会抖")
    print("  对照意义    : 与 CH0 的稳定性形成对比，说明时序该交给硬件外设")

    print()
    print("=" * 78)
    print("CH4  UART 帧")
    print("=" * 78)
    bit_us = 1e6 / UART_BAUD
    frame_us = bit_us * 10 * len(UART_MSG)
    print(f"  波特率      : {UART_BAUD} 8N1 → 1 bit = {bit_us:.3f} us")
    print(f"  一个字节    : 10 bit = {bit_us * 10:.2f} us（含起始位和停止位）")
    print(f"  文本        : {UART_MSG!r}，{len(UART_MSG)} 个字符")
    print(f"  一帧线上时长: {frame_us:.0f} us ≈ {frame_us / 1000:.2f} ms")
    print("  探测点      : 空闲为高电平，起始位把线拉低 —— 逻辑分析仪应能解码出文本")

    print()
    print("=" * 78)
    print("CH5/CH6  SPI 帧")
    print("=" * 78)
    print(f"  SCLK        : {SPI_CLOCK_HZ} Hz（周期 {1e6 / SPI_CLOCK_HZ:.2f} us）")
    print(f"  数据        : {' '.join(f'{b:02X}' for b in SPI_BYTES)} "
          f"（{len(SPI_BYTES)} 字节 = {len(SPI_BYTES) * 8} 个时钟）")
    print(f"  传输时长    : {len(SPI_BYTES) * 8 * 1e6 / SPI_CLOCK_HZ:.1f} us（不含帧间空隙）")
    print("  解码设置    : mode 0（CPOL=0, CPHA=0）、MSB first、无 CS（时钟门控）")
    print("  位序验证    : 0xAA=10101010, 0x55=01010101, 0xF0=11110000, 0x0F=00001111")

    print()
    print("=" * 78)
    print("时基与采样建议")
    print("=" * 78)
    print(f"  周期          : {BURST_PERIOD_MS} ms（每周期 {BURST_PERIOD_MS * SQUARE_FREQ_HZ // 1000} 个 CH0 方波）")
    print(f"  SYNC          : {SYNC_PULSE_US} us 高电平，上升沿作为触发点")
    rmt_total = sum(RMT_FIRST_PULSE_US + i * RMT_STEP_US + RMT_GAP_US
                    for i in range(RMT_PULSE_NUM)) + RMT_TAIL_US
    uart_offset = SYNC_PULSE_US + BURST_SYNC_TO_RMT_US + rmt_total + BURST_RMT_TO_UART_US
    spi_offset = uart_offset + BURST_UART_TO_SPI_US
    print(f"  相对偏移      : RMT +{BURST_SYNC_TO_RMT_US} us，"
          f"UART +{uart_offset} us（≈{uart_offset / 1000:.1f} ms），"
          f"SPI +{spi_offset} us（≈{spi_offset / 1000:.1f} ms）")
    print("  说明          : UART 帧写入环形缓冲后立即返回，所以 SPI 起点 = UART 起点 + 2 ms")
    print()
    print(f"  {'信号':<28}{'最快成分':>14}{'8× 采样率':>14}{'建议':>14}")
    rows = [
        ("CH0 1 kHz 方波", 1e3, 100e3),
        ("CH1 10 kHz PWM（10 us 脉宽）", 1e5, 2e6),
        ("CH2 RMT（2 us 脉宽）", 5e5, 12e6),
        ("CH3 bit-bang（约 300 kHz）", 3e5, 12e6),
        ("CH4 UART 115200（8.68 us/bit）", UART_BAUD, 2e6),
        ("CH5 SPI SCLK 1 MHz", SPI_CLOCK_HZ, 12e6),
        ("CH7 SYNC（10 us 脉冲）", 1e5, 2e6),
    ]
    for name, fastest, suggest in rows:
        print(f"  {name:<28}{fastest / 1e6:>11.3f} MHz{8 * fastest / 1e6:>11.2f} MHz"
              f"{suggest / 1e6:>11.1f} MHz")
    print()
    print("  一次采集抓全所有通道：12 MS/s + 1 M 采样点 = 83 ms 窗口 > 50 ms 周期 ✔")
    print("  若设备最高只到 8 MS/s：60 ms 窗口仍然够，但 2 us 脉冲只剩 16 个点")


if __name__ == "__main__":
    main()
