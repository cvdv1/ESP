#include "wave_ledc.h"

#include <stdint.h>

#include "wave_pins.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wave_ledc";

/* 只用低速模式：ESP32-S3 也只有这一组（高速模式是 ESP32 才有的老特性）。 */
#define LEDC_MODE LEDC_LOW_SPEED_MODE

/* 占空比计数值：n bit 分辨率 → 0 ~ 2^n-1 */
static uint32_t duty_max_count(uint32_t bits)
{
    return (1UL << bits) - 1UL;
}

/* 百分比 → 计数值，四舍五入。例：10% → (10*255+50)/100 = 26 */
static uint32_t pct_to_duty(uint32_t pct, uint32_t max_count)
{
    return (pct * max_count + 50) / 100;
}

/* ---------- CH0：1 kHz / 50% 固定方波 ---------- */
static void square_init(void)
{
    const uint32_t max_count = duty_max_count(SQUARE_DUTY_BITS);

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = (ledc_timer_bit_t)SQUARE_DUTY_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = SQUARE_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    const ledc_channel_config_t ch_cfg = {
        .gpio_num = WAVE_PIN_SQUARE_1K,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = max_count / 2 + 1, /* 1023/2+1 = 512，正好 50% */
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    ESP_LOGI(TAG, "CH0 GPIO%d: target %d Hz, 50%% (%d bit), real %lu Hz",
             WAVE_PIN_SQUARE_1K, SQUARE_FREQ_HZ, SQUARE_DUTY_BITS,
             (unsigned long)ledc_get_freq(LEDC_MODE, LEDC_TIMER_0));
}

/* ---------- CH1：10 kHz 载波 + 占空比三角扫描 ---------- */
static uint32_t s_duty_min; /* 10% 对应的计数值 */
static uint32_t s_duty_max; /* 90% 对应的计数值 */

static void pwm_sweep_init(void)
{
    const uint32_t max_count = duty_max_count(PWM_DUTY_BITS);
    s_duty_min = pct_to_duty(PWM_DUTY_MIN_PCT, max_count);
    s_duty_max = pct_to_duty(PWM_DUTY_MAX_PCT, max_count);

    /* 频率和 CH0 不同 → 必须用另一个 timer（LEDC_TIMER_1） */
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = (ledc_timer_bit_t)PWM_DUTY_BITS,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    const ledc_channel_config_t ch_cfg = {
        .gpio_num = WAVE_PIN_PWM_SWEEP,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = s_duty_min, /* 从 10% 起步 */
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    ESP_LOGI(TAG, "CH1 GPIO%d: target %d Hz (%d bit), duty %d%%..%d%% -> counts %lu..%lu, real %lu Hz",
             WAVE_PIN_PWM_SWEEP, PWM_FREQ_HZ, PWM_DUTY_BITS,
             PWM_DUTY_MIN_PCT, PWM_DUTY_MAX_PCT,
             (unsigned long)s_duty_min, (unsigned long)s_duty_max,
             (unsigned long)ledc_get_freq(LEDC_MODE, LEDC_TIMER_1));
}

/*
 * 每 PWM_SWEEP_STEP_MS 改一次占空比，按三角波 0 → 峰值 → 0 走一遍。
 * 全程整数运算，不碰浮点：
 *   half  = STEPS/2                  （本工程 100/2 = 50）
 *   tri   = step            (step < half)
 *         = STEPS-1-step    (step >= half)   → 取值 0 .. half-1 .. 0
 *   duty  = min + (max-min) * tri / (half-1) → 正好走到 10% 和 90%
 *
 * 这里用相对延时 vTaskDelay()，不用 vTaskDelayUntil()：时基任务（优先级更高）
 * 有一段 4 ms 的忙等，扫描任务可能被推迟几个 tick；用相对延时可以保证"被推迟
 * 就往后顺延"，而不是醒来后连跑好几步追赶，占空比的变化才是均匀的。
 */
static void sweep_task(void *arg)
{
    (void)arg;

    const TickType_t step_ticks = pdMS_TO_TICKS(PWM_SWEEP_STEP_MS);
    const uint32_t half = PWM_SWEEP_STEPS / 2;
    const uint32_t span_max = s_duty_max - s_duty_min;

    uint32_t step = 0;

    for (;;) {
        const uint32_t tri = (step < half) ? step : (PWM_SWEEP_STEPS - 1 - step);
        const uint32_t duty = s_duty_min + (span_max * tri) / (half - 1);

        /*
         * thread-safe 的"设置并立即更新"，避免两次调用之间出现半截波形。
         * 注意：这个 API 依赖 LEDC 的 fade 服务，必须先调 ledc_fade_func_install()，
         * 否则运行时会返回 ESP_FAIL 并打印 "Fade service not installed"（实测踩过）。
         */
        ESP_ERROR_CHECK(ledc_set_duty_and_update(LEDC_MODE, LEDC_CHANNEL_1, duty, 0));

        step = (step + 1U) % PWM_SWEEP_STEPS;
        vTaskDelay(step_ticks);
    }
}

void wave_ledc_init(void)
{
    square_init();
    pwm_sweep_init();

    /* ledc_set_duty_and_update() 依赖 fade 服务：不装就会在运行时返回 ESP_FAIL。
       0 = 用默认中断优先级；只需装一次，卸载用 ledc_fade_func_uninstall()。 */
    ESP_ERROR_CHECK(ledc_fade_func_install(0));

    /* 扫描任务放在 core 0，优先级低于突发任务；栈 3 KB 足够（只调驱动 API）。 */
    xTaskCreatePinnedToCore(sweep_task, "pwm_sweep", 3072, NULL, 4, NULL, 0);
}
