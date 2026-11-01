#include "wave_bus.h"

#include <stdint.h>

#include "wave_pins.h"

#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "wave_bus";

/* wave_pins.h 保持不 include 驱动头文件，这里做一次显式转换，便于移植到别的 UART */
#define WAVE_UART_PORT ((uart_port_t)UART_PORT_NUM)

/* ============================ UART ============================ */
static void uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(WAVE_UART_PORT, &cfg));

    /* 只发不收：RX 填 -1；TX 指定引脚。CS/RTS 不用。 */
    ESP_ERROR_CHECK(uart_set_pin(WAVE_UART_PORT, WAVE_PIN_UART_TX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* rx_buffer_size 必须 > FIFO(128)，给 256 即可；
       tx_buffer_size 非 0 → uart_write_bytes 拷进环形缓冲就返回，不阻塞任务。 */
    ESP_ERROR_CHECK(uart_driver_install(WAVE_UART_PORT, 256, 1024, 0, NULL, 0));

    ESP_LOGI(TAG, "CH4 GPIO%d: UART%d TX, %d 8N1",
             WAVE_PIN_UART_TX, UART_PORT_NUM, UART_BAUD_RATE);
}

void wave_bus_send_uart(void)
{
    /* 13 个字符 × 10 bit × 8.68 us ≈ 1.13 ms 的线上时间 */
    static const char msg[] = "ESP32-S3 LA\r\n";
    uart_write_bytes(WAVE_UART_PORT, msg, sizeof(msg) - 1);
}

/* ============================ SPI ============================ */
static spi_device_handle_t s_spi_dev = NULL;

static void spi_init(void)
{
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = WAVE_PIN_SPI_MOSI,
        .miso_io_num = -1, /* 只发不收 */
        .sclk_io_num = WAVE_PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    /* 只发 4 字节，不需要 DMA */
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_DISABLED));

    const spi_device_interface_config_t dev_cfg = {
        .mode = 0,           /* CPOL=0, CPHA=0：时钟空闲低电平，上升沿采样 */
        .clock_speed_hz = SPI_CLOCK_HZ,
        .spics_io_num = -1,  /* 不接片选，解码器里选 "no CS" 或用时钟门控 */
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi_dev));

    ESP_LOGI(TAG, "CH5/CH6 GPIO%d(SCLK)/GPIO%d(MOSI): SPI2 mode0, %d Hz",
             WAVE_PIN_SPI_SCLK, WAVE_PIN_SPI_MOSI, SPI_CLOCK_HZ);
}

void wave_bus_send_spi(void)
{
    static const uint8_t payload[] = {0xAA, 0x55, 0xF0, 0x0F};
    if (s_spi_dev == NULL) {
        return;
    }

    spi_transaction_t trans = {
        .length = sizeof(payload) * 8, /* 单位是 bit，不是 byte */
        .tx_buffer = payload,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi_dev, &trans));
}

void wave_bus_init(void)
{
    uart_init();
    spi_init();
}
