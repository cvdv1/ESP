/*
 * CH2：用 RMT（Remote Control Transceiver）外设输出"任意脉宽"的波形
 *
 * RMT 的本质：一块硬件 FIFO，里面存的是"电平 + 持续多少个 tick"的符号表，
 * 由硬件按时钟逐条打出，CPU 只在开始/结束时介入。所以它既没有软件抖动，
 * 又能描述 LEDC 表达不了的复杂时序（WS2812、DHT11、红外、舵机脉冲……）。
 *
 * 本例输出一簇 16 个方波，高电平脉宽从 2 us 逐个增加到 17 us，间隔 2 us。
 * 逻辑分析仪上就是一把"越来越宽的梳子"，用来看微秒级精度最直观。
 */
#pragma once

#include <stdint.h>

/* 创建 RMT 通道并预填符号表，随后使能通道。 */
void wave_rmt_init(void);

/* 触发一次突发（阻塞到发送完成，最长等待 100 ms）。 */
void wave_rmt_send(void);

/* 一簇突发的大致时长（微秒），用于启动日志和文档核对。 */
uint32_t wave_rmt_burst_us(void);
