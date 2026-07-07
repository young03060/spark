#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "sensor_task.h"
#include "lcd_display.h"
#include "alarm_task.h"
#include "cloud_task.h"

/* LCD BSP API (from libraries/Board_Drivers/lcd/drv_lcd.c) */
extern void lcd_clear(rt_uint16_t color);
extern void lcd_set_color(rt_uint16_t back, rt_uint16_t fore);
extern rt_err_t lcd_show_string(rt_uint16_t x, rt_uint16_t y,
                                rt_uint32_t size, const char *fmt, ...);

#define DBG_TAG    "main"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

/* LCD 刷新间隔 (ms) */
#define LCD_REFRESH_MS  2000

/* LCD 刷新线程 */
static rt_thread_t g_lcd_thread = RT_NULL;
static rt_bool_t   g_lcd_running = RT_FALSE;

/**
 * LCD 刷新线程入口
 * 每2秒从 sensor_task 获取最新数据并刷新屏幕
 */
static void lcd_refresh_thread(void *parameter)
{
    struct env_sensor_data data;

    LOG_I("LCD refresh thread started");

    while (g_lcd_running)
    {
        /* 获取最新传感器数据 */
        if (sensor_get_latest(&data) == RT_EOK)
        {
            /* 刷新 LCD 显示 */
            lcd_display_update(&data);
        }
        else
        {
            LOG_W("Failed to get sensor data");
        }

        rt_thread_mdelay(LCD_REFRESH_MS);
    }
}

/**
 * 主函数 (RT-Thread 自动以线程方式启动)
 *
 * RT-Thread main 线程优先级默认为 10
 * 栈大小在 rtconfig.h 中配置 (RT_MAIN_THREAD_STACK_SIZE)
 */
int main(void)
{
    LOG_I("========================================");
    LOG_I("  SparkFire-1 Environment Monitor");
    LOG_I("  MCU:  STM32F407ZGT6 @168MHz");
    LOG_I("  OS:   RT-Thread v%s", RT_VERSION);
    LOG_I("  Sensors: AHT10 + PMS5003");
    LOG_I("  Display: 1.3\" ST7789V3 240x240 LCD");
    LOG_I("  Alarm:   LED(温度颜色) + Buzzer 10s");
    LOG_I("========================================");

    /* ---- 0. 开启 LCD 背光 (PF9) ---- */
    {
        #define LCD_BL_PIN  GET_PIN(F, 9)
        rt_pin_mode(LCD_BL_PIN, PIN_MODE_OUTPUT);
        rt_pin_write(LCD_BL_PIN, PIN_HIGH);
        LOG_I("LCD backlight: PF9 HIGH");
    }

    /* ---- 0.1 LCD 硬件测试 (RGB 三色闪烁) ---- */
    LOG_I("LCD self-test: RED -> GREEN -> BLUE");
    lcd_clear(LCD_COLOR_RED);
    rt_thread_mdelay(400);
    lcd_clear(LCD_COLOR_GREEN);
    rt_thread_mdelay(400);
    lcd_clear(LCD_COLOR_BLUE);
    rt_thread_mdelay(400);
    LOG_I("LCD self-test done");

    /* ---- 1. 初始化 LCD 显示 ---- */
    lcd_display_init();

    /* 显示启动画面 (持续1秒) */
    lcd_display_show_logo();
    rt_thread_mdelay(1500);

    /* 重新初始化显示界面 */
    lcd_display_init();

    /* ---- 2. 启动传感器采集任务 ---- */
    if (sensor_task_init() != RT_EOK)
    {
        LOG_E("Sensor task init failed!");

        /* LCD 显示错误信息 */
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_RED);
        lcd_show_string(30, 100, 16, "Sensor Init Failed!");
        lcd_show_string(20, 120, 16, "Check connections");

        return -1;
    }

    /* 等待首次传感器数据 (给传感器2秒预热) */
    LOG_I("Waiting for sensor warm-up...");
    rt_thread_mdelay(2000);

    /* ---- 3. 启动报警监控任务 (蜂鸣器) ---- */
    if (alarm_task_init() != RT_EOK)
    {
        LOG_E("Alarm task init failed!");
    }
    else
    {
        LOG_I("Alarm monitoring started");
    }

    /* ---- 3.5 启动云平台数据上传任务 (串口CSV) ---- */
    if (cloud_task_init() != RT_EOK)
    {
        LOG_E("Cloud task init failed!");
    }
    else
    {
        LOG_I("Cloud upload started (serial CSV mode)");
    }

    /* ---- 4. 启动 LCD 刷新线程 ---- */
    g_lcd_running = RT_TRUE;
    g_lcd_thread = rt_thread_create(
        "lcd_refresh",
        lcd_refresh_thread,
        RT_NULL,
        2048,   /* 栈大小 */
        12,     /* 优先级 (低于 main 线程) */
        20);    /* 时间片 */

    if (g_lcd_thread != RT_NULL)
    {
        rt_thread_startup(g_lcd_thread);
        LOG_I("LCD refresh thread created");
    }
    else
    {
        LOG_E("Failed to create LCD refresh thread");

        /* 降级方案: 在主循环中刷新 */
        struct env_sensor_data data;
        while (1)
        {
            if (sensor_get_latest(&data) == RT_EOK)
            {
                lcd_display_update(&data);
            }
            rt_thread_mdelay(LCD_REFRESH_MS);
        }
    }

    LOG_I("System running. Enjoy!");
    return 0;
}

/* ---- UART2 硬件层测试命令 (轮询方式，不依赖驱动) ---- */
#ifdef RT_USING_FINSH
#include <finsh.h>

static void cmd_uart2_test(int argc, char *argv[])
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    UART_HandleTypeDef huart2;
    uint8_t rx_buf[64];
    uint32_t rx_count = 0;
    uint32_t timeout_ticks;

    rt_kprintf("\n=== UART2 Hardware Test (Polling) ===\n");

    /* 1. Enable clocks */
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    rt_kprintf("Clocks enabled\n");

    /* 2. GPIO: PA2=TX(AF7), PA3=RX(AF7) */
    GPIO_InitStruct.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    rt_kprintf("PA2/PA3 -> USART2 AF7\n");

    /* 3. Init USART2 */
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 9600;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart2);
    rt_kprintf("USART2 9600-8N1 RX only\n");

    /* 4. Poll for data (5 seconds) */
    rt_kprintf("Polling for PMS5003 data (5s)...\n");
    timeout_ticks = rt_tick_get() + rt_tick_from_millisecond(5000);

    while (rt_tick_get() < timeout_ticks)
    {
        if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE))
        {
            uint8_t ch = (uint8_t)(huart2.Instance->DR & 0xFF);
            if (rx_count < sizeof(rx_buf))
                rx_buf[rx_count] = ch;
            rx_count++;
            __HAL_UART_CLEAR_FLAG(&huart2, UART_FLAG_RXNE);
        }
        /* Check for errors */
        if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_ORE | UART_FLAG_FE | UART_FLAG_NE))
        {
            __HAL_UART_CLEAR_FLAG(&huart2, UART_FLAG_ORE | UART_FLAG_FE | UART_FLAG_NE);
            rt_kprintf("E"); /* indicate error */
        }
    }

    /* 5. Report */
    rt_kprintf("\n--- Result ---\n");
    rt_kprintf("Bytes received: %lu\n", rx_count);

    if (rx_count > 0)
    {
        rt_kprintf("Hex: ");
        for (uint32_t i = 0; i < (rx_count < 32 ? rx_count : 32); i++)
            rt_kprintf("%02X ", rx_buf[i]);
        rt_kprintf("\n");

        if (rx_buf[0] == 0x42 && rx_count >= 2 && rx_buf[1] == 0x4D)
            rt_kprintf(">>> PMS5003 FRAME DETECTED! <<<\n");
        else
            rt_kprintf("Not a PMS5003 frame (expected 0x42 0x4D start)\n");
    }
    else
    {
        rt_kprintf("NO DATA! Please verify:\n");
        rt_kprintf("  PMS5003 VCC -> board 5V pin\n");
        rt_kprintf("  PMS5003 GND -> board GND pin\n");
        rt_kprintf("  PMS5003 TX  -> board PA3 pin\n");
        rt_kprintf("  PMS5003 needs ~30s warmup after power-on\n");
    }

    /* Cleanup: deinit so original driver can reclaim */
    HAL_UART_DeInit(&huart2);
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_2 | GPIO_PIN_3);
    rt_kprintf("=== End ===\n\n");
}
MSH_CMD_EXPORT(cmd_uart2_test, hardware-level UART2 RX test for PMS5003);

/**
 * 命令行: sensor 查看当前传感器数据
 * 在串口终端输入 sensor 即可查看
 */
static void cmd_sensor(int argc, char *argv[])
{
    struct env_sensor_data data;

    if (sensor_get_latest(&data) != RT_EOK)
    {
        rt_kprintf("Failed to read sensor data\n");
        return;
    }

    rt_kprintf("\n");
    rt_kprintf("========== Sensor Data ==========\n");

    /* Temperature & Humidity */
    if (data.temp_valid)
        rt_kprintf(" Temperature:  %d.%d C\n",
                   (int)data.temperature,
                   (int)(data.temperature * 10) % 10);
    else
        rt_kprintf(" Temperature:  (?) --.- C\n");

    if (data.humi_valid)
        rt_kprintf(" Humidity:     %d.%d %%\n",
                   (int)data.humidity,
                   (int)(data.humidity * 10) % 10);
    else
        rt_kprintf(" Humidity:     (?) --.- %%\n");

    /* PM data */
    if (data.pm_valid)
    {
        rt_kprintf("\n--- PMS5003 Detailed Data ---\n");
        rt_kprintf(" CF=1 (std):  PM1.0=%3d  PM2.5=%3d  PM10=%3d ug/m3\n",
                   data.pms.pm1_0_cf, data.pms.pm2_5_cf, data.pms.pm10_cf);
        rt_kprintf(" Atmospheric:  PM1.0=%3d  PM2.5=%3d  PM10=%3d ug/m3\n",
                   data.pms.pm1_0_atm, data.pms.pm2_5_atm, data.pms.pm10_atm);
        rt_kprintf("\n--- Particle Counts (/0.1L) ---\n");
        rt_kprintf(" >0.3um=%5d   >0.5um=%5d   >1.0um=%5d\n",
                   data.pms.n_0_3um, data.pms.n_0_5um, data.pms.n_1_0um);
        rt_kprintf(" >2.5um=%5d   >5.0um=%5d   >10um =%5d\n",
                   data.pms.n_2_5um, data.pms.n_5_0um, data.pms.n_10um);
        rt_kprintf("\n Air Quality: %s\n", quality_string(data.quality));
    }
    else
    {
        rt_kprintf(" PM2.5:        (?) --- ug/m3\n");
        rt_kprintf(" PM10:         (?) --- ug/m3\n");
        rt_kprintf(" Air Quality:  (no PM data)\n");
    }

    rt_kprintf("---------------------------------\n");
    rt_kprintf(" Sensor status: AHT=%s  PMS=%s\n",
               data.temp_valid ? "OK" : "FAIL",
               data.pm_valid   ? "OK" : "FAIL");
    rt_kprintf("=================================\n\n");
}
MSH_CMD_EXPORT(cmd_sensor, show current sensor data);

/**
 * 命令行: lcd_reset 重置 LCD 显示
 */
static void cmd_lcd_reset(int argc, char *argv[])
{
    lcd_display_init();
    rt_kprintf("LCD display reset\n");
}
MSH_CMD_EXPORT(cmd_lcd_reset, reset LCD display);

#endif /* RT_USING_FINSH */
