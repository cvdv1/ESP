/*
 * CH4 / CH5 / CH6：用真正的协议外设输出"能被解码器读懂"的帧
 *
 *   UART1 TX  -> 115200 8N1，文本 "ESP32-S3 LA\r\n"
 *   SPI2 SCLK -> 1 MHz
 *   SPI2 MOSI -> 0xAA 0x55 0xF0 0x0F（MSB first）
 *
 * 这一类波形的意义：不只是"看边沿"，还能在逻辑分析仪里挂上协议解码器，
 * 直接比对"我发出去的字节"和"解码出来的字节"是否一致。
 */
#pragma once

/* 初始化 UART1（仅 TX）和 SPI2（仅 SCLK/MOSI，无 CS）。 */
void wave_bus_init(void);

/* 发送一帧 UART 文本（写入 TX 缓冲后立即返回）。 */
void wave_bus_send_uart(void);

/* 发送 4 字节 SPI 数据（轮询方式，阻塞到发送完成）。 */
void wave_bus_send_spi(void);
