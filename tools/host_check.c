/*
 * 主机侧算术自检（不需要 ESP-IDF，也不需要开发板）
 *
 * 它只 include 真实的 main/wave_pins.h，用纯整数运算复算：
 *   1) LEDC 占空比扫描是否真的走满 10%~90%，且上下坡单调；
 *   2) RMT 突发的总时长、符号数是否在一个内存块内；
 *   3) 时基偏移是否小于周期。
 *
 * 用任意主机 gcc 编译即可（Windows 上可用 MinGW / VS Code 自带的那份）：
 *   gcc -std=gnu17 -O2 -Wall -Wextra -I main tools/host_check.c -o host_check
 *   ./host_check
 *
 * 改了 wave_pins.h 之后重跑一次，能立刻发现"改错了参数"这种低级错误。
 */
#include <stdio.h>

#include "wave_pins.h"

/* 与 wave_ledc.c 里的实现保持一致 */
static unsigned long pct_to_duty(unsigned long pct, unsigned long max_count)
{
    return (pct * max_count + 50) / 100;
}

int main(void)
{
    int failures = 0;

    /* ---------- 1) LEDC 占空比三角扫描 ---------- */
    const unsigned long full = 1UL << PWM_DUTY_BITS;
    const unsigned long max_count = full - 1UL;
    const unsigned long dmin = pct_to_duty(PWM_DUTY_MIN_PCT, max_count);
    const unsigned long dmax = pct_to_duty(PWM_DUTY_MAX_PCT, max_count);
    const unsigned long span = dmax - dmin;
    const unsigned long half = PWM_SWEEP_STEPS / 2;

    unsigned long prev = 0, valley = 1000000, peak = 0;
    int up_ok = 1, down_ok = 1, descending = 0;

    for (unsigned long step = 0; step < PWM_SWEEP_STEPS; step++) {
        const unsigned long tri = (step < half) ? step : (PWM_SWEEP_STEPS - 1 - step);
        const unsigned long duty = dmin + (span * tri) / (half - 1);

        if (duty > max_count) {
            printf("[FAIL] step %lu: duty %lu 超过计数值上限 %lu\n", step, duty, max_count);
            failures++;
        }
        if (step > 0) {
            if (!descending) {
                if (duty < prev) {
                    up_ok = 0;
                }
            } else if (duty > prev) {
                down_ok = 0;
            }
        }
        if (step >= half) {
            descending = 1;
        }
        if (duty < valley) {
            valley = duty;
        }
        if (duty > peak) {
            peak = duty;
        }
        prev = duty;
    }

    printf("LEDC 扫描: 计数值 %lu..%lu / %lu  ->  %.2f%% .. %.2f%%\n",
           dmin, dmax, full, 100.0 * dmin / (double)full, 100.0 * dmax / (double)full);
    printf("           实际扫描到 %lu..%lu, 上坡 %s, 下坡 %s, 周期 %lu ms\n",
           valley, peak, up_ok ? "单调" : "不单调!", down_ok ? "单调" : "不单调!",
           (unsigned long)(PWM_SWEEP_STEPS * PWM_SWEEP_STEP_MS));
    if (!up_ok || !down_ok || valley != dmin || peak != dmax) {
        printf("[FAIL] 三角波没有扫满 10%%..90%%（检查 PWM_SWEEP_STEPS 是否为偶数、分母是否为 half-1）\n");
        failures++;
    }

    /* ---------- 2) RMT 突发 ---------- */
    const unsigned long sum_high = (unsigned long)RMT_PULSE_NUM * RMT_FIRST_PULSE_US
        + (unsigned long)RMT_STEP_US * (RMT_PULSE_NUM * (RMT_PULSE_NUM - 1) / 2);
    const unsigned long burst_us = sum_high + (unsigned long)RMT_PULSE_NUM * RMT_GAP_US + RMT_TAIL_US;

    printf("RMT 突发 : %lu us, 符号数 %d / 内存块 %d\n",
           burst_us, RMT_PULSE_NUM + 1, RMT_MEM_BLOCK_SYMBOLS);
    if (burst_us > (unsigned long)RMT_PULSE_NUM * 32767UL) {
        printf("[FAIL] RMT 单段超过 32767 tick\n");
        failures++;
    }
    if (RMT_PULSE_NUM + 1 > RMT_MEM_BLOCK_SYMBOLS) {
        printf("[FAIL] RMT 符号数超过一个内存块\n");
        failures++;
    }

    /* ---------- 3) 时基 ---------- */
    const unsigned long total_us = (unsigned long)BURST_SYNC_TO_RMT_US
        + BURST_RMT_TO_UART_US + BURST_UART_TO_SPI_US;
    printf("时基     : SYNC 后 RMT +%d us；编程延时 UART +%lu us、SPI +%lu us，周期 %d ms\n",
           BURST_SYNC_TO_RMT_US, (unsigned long)BURST_SYNC_TO_RMT_US + BURST_RMT_TO_UART_US,
           total_us, BURST_PERIOD_MS);
    printf("           实际起点约 +2.0 ms / +4.0 ms（比编程延时多出的 %lu us 就是 RMT 突发本身）\n",
           burst_us);
    if (total_us >= (unsigned long)BURST_PERIOD_MS * 1000UL) {
        printf("[FAIL] 时基偏移超过周期\n");
        failures++;
    }

    printf("\n%s\n", failures == 0 ? "PASS：参数算术自洽" : "FAIL：见上面的提示");
    return failures == 0 ? 0 : 1;
}
