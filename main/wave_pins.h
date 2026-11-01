/*
 * 逻辑分析仪波形演示 —— 引脚与参数集中配置
 *
 * 只改这个文件就能换引脚 / 换频率 / 换占空比，其它 .c 文件不用动。
 *
 * ===== 为什么选这几个 GPIO（ESP32-S3）=====
 *   GPIO26~32    接 SPI Flash，绝对不能用
 *   GPIO33~37    八线 PSRAM 模组上被 PSRAM 占用
 *   GPIO19/20    USB D-/D+（走 USB-Serial-JTAG 时不能用）
 *   GPIO39~42    JTAG（GPIO39/40 是 MTCK/MTDO，默认调试脚）
 *   GPIO0/3/45/46 strapping 脚，上电瞬间会被外部电路拉高/拉低，做输出会看到毛刺
 * 本工程用 GPIO4/5/6/7/15/16/17/18，都是普通 GPIO，上电无特殊作用。
 *
 * 注意：引脚只负责"输出"，逻辑分析仪真正的参考地必须是开发板 GND（共地），
 *       否则测到的要么全是高，要么是浮空噪声。
 */
#pragma once

/* ===================== 引脚分配 ===================== */
#define WAVE_PIN_SQUARE_1K   4    /* CH0：LEDC 1 kHz 方波（最稳的参考时钟） */
#define WAVE_PIN_PWM_SWEEP   5    /* CH1：LEDC 10 kHz 载波 + 占空比三角扫描 */
#define WAVE_PIN_RMT_BURST   6    /* CH2：RMT 任意脉宽突发（微秒级精度） */
#define WAVE_PIN_BITBANG     7    /* CH3：软件 bit-bang，用来对比"抖动" */
#define WAVE_PIN_UART_TX    15    /* CH4：UART1 TX，用异步串口解码器看文本 */
#define WAVE_PIN_SPI_SCLK   16    /* CH5：SPI2 时钟 */
#define WAVE_PIN_SPI_MOSI   17    /* CH6：SPI2 数据 */
#define WAVE_PIN_SYNC       18    /* CH7：触发用的同步脉冲 */

/* ===================== CH0：1 kHz / 50% 方波 ===================== */
#define SQUARE_FREQ_HZ       1000  /* 目标频率 */
#define SQUARE_DUTY_BITS     10    /* 占空比分辨率 10 bit → 计数值 0~1023 */
/* 提示：LEDC 频率上限约 80 MHz / 2^分辨率，10 kHz 以上别用 14 bit */

/* ===================== CH1：10 kHz 载波 + 占空比扫描 ===================== */
#define PWM_FREQ_HZ          10000 /* 载波频率（逻辑分析仪看的是这个周期 100 us） */
#define PWM_DUTY_BITS        8     /* 8 bit → 计数值 0~255，10% = 26，90% = 230 */
#define PWM_DUTY_MIN_PCT     10    /* 最小占空比（%） */
#define PWM_DUTY_MAX_PCT     90    /* 最大占空比（%） */
#define PWM_SWEEP_STEP_MS    10    /* 每 10 ms 更新一次占空比 */
#define PWM_SWEEP_STEPS      100   /* 100 步 × 10 ms = 1 s 一个三角波周期 */

/* ===================== CH2：RMT 任意脉宽突发 ===================== */
#define RMT_RESOLUTION_HZ    1000000 /* RMT 时钟 1 MHz → 1 tick = 1 us（量化误差 ±1 us） */
#define RMT_PULSE_NUM        16      /* 一簇 16 个脉冲 */
#define RMT_FIRST_PULSE_US   2       /* 第 1 个高电平 2 us */
#define RMT_STEP_US          1       /* 每个脉冲递增 1 us → 2,3,4 ... 17 us */
#define RMT_GAP_US           2       /* 脉冲之间的低电平 2 us */
#define RMT_TAIL_US          10      /* 突发结尾拉低 10 us，方便看清簇的边界 */
#define RMT_MEM_BLOCK_SYMBOLS 48     /* S3 每个 RMT 通道 48 个 symbol，正好一个 block */

/* ===================== CH4：UART 文本帧 ===================== */
#define UART_PORT_NUM        1       /* UART1（UART0 留给串口控制台/日志） */
#define UART_BAUD_RATE       115200

/* ===================== CH5/CH6：SPI 帧 ===================== */
#define SPI_CLOCK_HZ         1000000 /* 1 MHz → 时钟周期 1 us */

/* ===================== 时基（SYNC 与各突发的时间关系）===================== */
#define BURST_PERIOD_MS      50   /* 每 50 ms 重复一个完整演示周期 */
#define SYNC_PULSE_US        10   /* SYNC 高电平 10 us：足够任何采样率抓到 */
#define BURST_SYNC_TO_RMT_US 500  /* SYNC 上升沿 → RMT 突发起点 */
#define BURST_RMT_TO_UART_US 1300 /* RMT 突发结束 → UART 帧起点（累计约 2 ms） */
#define BURST_UART_TO_SPI_US 2000 /* UART 帧结束 → SPI 帧起点（累计约 4 ms） */

/* ===================== CH3：软件方波 ===================== */
#define BITBANG_HALF_US      1    /* 每个半周期延时 1 us → 理论上 500 kHz，
                                     实际受函数调用开销影响，约 250~500 kHz 且会抖动 */

/* ===================== 参数自检（编译期断言）=====================
 * 改参数改错了，直接在编译阶段失败并给出提示，而不是烧进去之后
 * 在逻辑分析仪上盯着一个莫名其妙的波形猜。 */
_Static_assert(SQUARE_DUTY_BITS >= 1 && SQUARE_DUTY_BITS <= 14, "LEDC 分辨率只能是 1~14 bit");
_Static_assert(PWM_DUTY_BITS >= 1 && PWM_DUTY_BITS <= 14, "LEDC 分辨率只能是 1~14 bit");
_Static_assert(((1UL << SQUARE_DUTY_BITS) - 1) / 2 + 1 == (1UL << (SQUARE_DUTY_BITS - 1)),
               "CH0 占空比不等于 50%");
_Static_assert(SQUARE_FREQ_HZ <= 80000000UL / (1UL << SQUARE_DUTY_BITS),
               "CH0 频率超过该分辨率的 LEDC 上限（80MHz / 2^bits）");
_Static_assert(PWM_FREQ_HZ <= 80000000UL / (1UL << PWM_DUTY_BITS),
               "CH1 频率超过该分辨率的 LEDC 上限（80MHz / 2^bits）");
_Static_assert(PWM_SWEEP_STEPS >= 2, "占空比扫描至少需要 2 步");
_Static_assert(PWM_SWEEP_STEPS % 2 == 0 && PWM_SWEEP_STEPS >= 4,
               "三角波的步数必须是偶数且不少于 4（否则扫描不到满量程）");
_Static_assert(RMT_MEM_BLOCK_SYMBOLS % 2 == 0 && RMT_MEM_BLOCK_SYMBOLS >= 48,
               "RMT mem_block_symbols 必须为偶数，且不少于 48（ESP32-S3 每通道 48 words）");
_Static_assert(RMT_PULSE_NUM + 1 <= RMT_MEM_BLOCK_SYMBOLS,
               "RMT 符号数超过一个内存块，需增大 RMT_MEM_BLOCK_SYMBOLS 或启用 DMA");
_Static_assert(RMT_FIRST_PULSE_US + (RMT_PULSE_NUM - 1) * RMT_STEP_US <= 32767,
               "RMT 单个电平段最长 32767 个 tick");
_Static_assert(BURST_SYNC_TO_RMT_US + BURST_RMT_TO_UART_US + BURST_UART_TO_SPI_US
                   < BURST_PERIOD_MS * 1000,
               "时基内的偏移总和超过了周期，突发会追不上下一轮");
