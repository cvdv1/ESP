/*
 * ESP32-S3 波形演示 —— 主程序
 *
 * 一条 50 ms 的时基把所有通道串起来，方便逻辑分析仪"单次采集 + 一屏看完"：
 *
 *   t = 0 ms      CH7 SYNC 输出 10 us 高脉冲（触发基准）
 *   t = 0.5 ms    CH2 RMT 突发（16 个 2..17 us 的脉冲，约 194 us 结束）
 *   t = 2 ms      CH4 UART 帧 "ESP32-S3 LA\r\n"（约 1.13 ms 发完）
 *   t = 4 ms      CH5/CH6 SPI 帧（4 字节 @1 MHz）
 *
 *   全程常开：CH0 1 kHz 方波、CH1 10 kHz 占空比扫描、CH3 软件 bit-bang。
 *
 * 两个任务：
 *   burst_task   （core 0）50 ms 时基，顺序触发 RMT / UART / SPI
 *   bitbang_task （core 1）死循环翻转 GPIO，用来看"软件时序"的抖动
 */
#include <stdio.h>

#include "wave_bus.h"
#include "wave_ledc.h"
#include "wave_pins.h"
#include "wave_rmt.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "la_wave";

/* 把一个引脚配成推挽输出（不开上下拉：外部逻辑分析仪是高阻输入，无需上拉）。 */
static void output_pin_init(int pin)
{
    const gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)pin, 0));
}

/*
 * 50 ms 时基任务。
 *
 * 注意：这里用 esp_rom_delay_us() 做微秒级延时，会阻塞本任务约 4 ms。
 * 因为周期是 50 ms，余量足够；而且 LEDC/bit-bang 都是别的任务/硬件在跑，
 * 不会因为这个阻塞而变形。
 */
static void burst_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(BURST_PERIOD_MS);

    while (1) {
        /* ---- 1) 触发脉冲：逻辑分析仪在 CH7 上设"上升沿"触发 ---- */
        gpio_set_level((gpio_num_t)WAVE_PIN_SYNC, 1);
        esp_rom_delay_us(SYNC_PULSE_US);
        gpio_set_level((gpio_num_t)WAVE_PIN_SYNC, 0);

        /* ---- 2) SYNC 后 500 us：RMT 任意脉宽突发 ---- */
        esp_rom_delay_us(BURST_SYNC_TO_RMT_US);
        wave_rmt_send();

        /* ---- 3) 再 1.3 ms：UART 帧起点约在 SYNC 后 2 ms ---- */
        esp_rom_delay_us(BURST_RMT_TO_UART_US);
        wave_bus_send_uart();

        /* ---- 4) 再 2 ms：SPI 帧起点约在 SYNC 后 4 ms ---- */
        esp_rom_delay_us(BURST_UART_TO_SPI_US);
        wave_bus_send_spi();

        vTaskDelayUntil(&last_wake, period);
    }
}

/*
 * 软件 bit-bang 任务：完全不经过外设，靠 CPU 一条条改 GPIO。
 *
 * 结果是"能出方波，但频率和占空比都不准"——这正是要用逻辑分析仪对比的地方：
 * 同样一根探针，CH0（硬件 PWM）稳如直线，CH3（软件）每次边沿都在抖。
 */
static void bitbang_task(void *arg)
{
    (void)arg;
    while (1) {
        gpio_set_level((gpio_num_t)WAVE_PIN_BITBANG, 1);
        esp_rom_delay_us(BITBANG_HALF_US);
        gpio_set_level((gpio_num_t)WAVE_PIN_BITBANG, 0);
        esp_rom_delay_us(BITBANG_HALF_US);
    }
}

static void print_pin_table(void)
{
    ESP_LOGI(TAG, "==================== 引脚 / 波形速查 ====================");
    ESP_LOGI(TAG, "CH0 GPIO%-2d 1 kHz 50%% 方波（LEDC 硬件）", WAVE_PIN_SQUARE_1K);
    ESP_LOGI(TAG, "CH1 GPIO%-2d 10 kHz 载波，占空比 10%%~90%% 每 1 s 三角扫描", WAVE_PIN_PWM_SWEEP);
    ESP_LOGI(TAG, "CH2 GPIO%-2d RMT 突发：16 个脉冲，高电平 2~17 us，间隔 2 us", WAVE_PIN_RMT_BURST);
    ESP_LOGI(TAG, "CH3 GPIO%-2d 软件 bit-bang 方波（约 250~500 kHz，有抖动）", WAVE_PIN_BITBANG);
    ESP_LOGI(TAG, "CH4 GPIO%-2d UART1 TX 115200 8N1，文本 ESP32-S3 LA", WAVE_PIN_UART_TX);
    ESP_LOGI(TAG, "CH5 GPIO%-2d SPI2 SCLK 1 MHz", WAVE_PIN_SPI_SCLK);
    ESP_LOGI(TAG, "CH6 GPIO%-2d SPI2 MOSI 0xAA 0x55 0xF0 0x0F", WAVE_PIN_SPI_MOSI);
    ESP_LOGI(TAG, "CH7 GPIO%-2d SYNC：每 %d ms 一个 %d us 高脉冲（用上升沿触发）",
             WAVE_PIN_SYNC, BURST_PERIOD_MS, SYNC_PULSE_US);
    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, "所有通道都相对 SYNC：RMT 约 +%d us，UART 约 +2.0 ms，SPI 约 +4.0 ms",
             BURST_SYNC_TO_RMT_US);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 多波形演示启动（目标：8 通道逻辑分析仪一次抓完）");

    /* 1) 先让两个"纯 GPIO"通道进入已知电平，避免上电瞬间的浮空毛刺 */
    output_pin_init(WAVE_PIN_SYNC);
    output_pin_init(WAVE_PIN_BITBANG);

    /* 2) 硬件外设：LEDC 方波 + 占空比扫描（内部会创建扫描任务） */
    wave_ledc_init();

    /* 3) 硬件外设：RMT 任意脉宽 */
    wave_rmt_init();

    /* 4) 协议外设：UART + SPI */
    wave_bus_init();

    /* 5) 时基任务与 bit-bang 任务分核运行，避免互相抢 CPU */
    xTaskCreatePinnedToCore(burst_task, "wave_burst", 4096, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(bitbang_task, "wave_bitbang", 2048, NULL, 5, NULL, 1);

    print_pin_table();

    ESP_LOGI(TAG, "输出已开始。逻辑分析仪：采样率建议 12 MHz，CH7 上升沿触发，单次采集。");
}
