#include "wave_rmt.h"

#include "wave_pins.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "wave_rmt";

/* 符号表：前 RMT_PULSE_NUM 条是"高电平 + 间隔"，最后 1 条是收尾的低电平。 */
static rmt_symbol_word_t s_symbols[RMT_PULSE_NUM + 1];
static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_encoder = NULL;

/* 符号数量必须装得进一个内存块，否则 rmt_transmit 会报错。 */
_Static_assert(RMT_PULSE_NUM + 1 <= RMT_MEM_BLOCK_SYMBOLS,
               "RMT burst exceeds one memory block, increase RMT_MEM_BLOCK_SYMBOLS");

/*
 * 填符号表：第 i 条 = 高电平 (FIRST + i*STEP) us，然后低电平 GAP us。
 * RMT 每个 symbol 由两段组成（level0/duration0 + level1/duration1），
 * 正好对应"一个高电平 + 一个低电平"，所以 16 个脉冲只需 16 个 symbol。
 */
static void build_symbols(void)
{
    for (int i = 0; i < RMT_PULSE_NUM; i++) {
        const uint32_t high_us = RMT_FIRST_PULSE_US + (uint32_t)i * RMT_STEP_US;

        s_symbols[i].level0 = 1;
        s_symbols[i].duration0 = (uint16_t)high_us;
        s_symbols[i].level1 = 0;
        s_symbols[i].duration1 = (uint16_t)RMT_GAP_US;
    }

    /* 收尾：把电平钉在低，时间对半放在两个半段里（RMT 要求两段都有值）。 */
    s_symbols[RMT_PULSE_NUM].level0 = 0;
    s_symbols[RMT_PULSE_NUM].duration0 = (uint16_t)(RMT_TAIL_US / 2);
    s_symbols[RMT_PULSE_NUM].level1 = 0;
    s_symbols[RMT_PULSE_NUM].duration1 = (uint16_t)(RMT_TAIL_US / 2);
}

void wave_rmt_init(void)
{
    build_symbols();

    const rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = WAVE_PIN_RMT_BURST,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &s_chan));

    /* copy encoder：不做任何转换，直接把符号表原样打进 FIFO，
       正合适"我想精确控制每一段脉宽"的场景。 */
    rmt_copy_encoder_config_t copy_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_cfg, &s_encoder));

    ESP_ERROR_CHECK(rmt_enable(s_chan));

    ESP_LOGI(TAG, "CH2 GPIO%d: %d pulses, %d..%d us high, %d us gap, burst ~%lu us, res %lu Hz",
             WAVE_PIN_RMT_BURST, RMT_PULSE_NUM,
             RMT_FIRST_PULSE_US, RMT_FIRST_PULSE_US + (RMT_PULSE_NUM - 1) * RMT_STEP_US,
             RMT_GAP_US, (unsigned long)wave_rmt_burst_us(),
             (unsigned long)RMT_RESOLUTION_HZ);
}

uint32_t wave_rmt_burst_us(void)
{
    /* 每个脉冲 = 高电平 + 间隔；最后再加收尾的低电平 */
    const uint32_t sum_high = (uint32_t)RMT_PULSE_NUM * RMT_FIRST_PULSE_US
                              + (uint32_t)RMT_STEP_US * (RMT_PULSE_NUM * (RMT_PULSE_NUM - 1) / 2);
    return sum_high + (uint32_t)RMT_PULSE_NUM * RMT_GAP_US + RMT_TAIL_US;
}

void wave_rmt_send(void)
{
    if (s_chan == NULL) {
        return;
    }

    const rmt_transmit_config_t tx_cfg = {
        .loop_count = 0, /* 0 = 只发一次，不循环 */
    };

    const esp_err_t err = rmt_transmit(s_chan, s_encoder, s_symbols,
                                       sizeof(s_symbols), &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
        return;
    }

    /* 等硬件发完，避免上一簇还没结束就覆盖符号表 */
    const esp_err_t wait_err = rmt_tx_wait_all_done(s_chan, 100);
    if (wait_err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_tx_wait_all_done timeout: %s", esp_err_to_name(wait_err));
    }
}
