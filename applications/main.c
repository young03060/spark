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

    /* ---- 3.5 启动云平台数据上传任务 (RW007 WiFi + OneNET MQTT) ---- */
    if (cloud_task_init() != RT_EOK)
    {
        LOG_E("Cloud task init failed!");
    }
    else
    {
        LOG_I("Cloud upload started (WiFi + OneNET MQTT mode)");
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

/* ═══════════════════════════════════════════════════════════
 * W5500 Hardware Diagnostic Command
 * Tests GPIO wiring + SPI communication by reading W5500 VERSIONR register.
 * Expected: VERSIONR = 0x04 (W5500 chip ID)
 * ═══════════════════════════════════════════════════════════ */
#include <rtdevice.h>

/* W5500 GPIO pins (must match platformW5500Hardware.c) */
#define W5500_SCLK  GET_PIN(G, 5)
#define W5500_MOSI  GET_PIN(G, 1)
#define W5500_MISO  GET_PIN(G, 3)
#define W5500_SCS   GET_PIN(G, 6)
#define W5500_RST   GET_PIN(G, 4)

/* Software SPI helpers */
static void w5500_spi_delay(void) { for(volatile int i=0; i<20; i++) __NOP(); }

static void w5500_spi_write(uint8_t data)
{
    for (int i = 7; i >= 0; i--) {
        rt_pin_write(W5500_SCLK, PIN_LOW);
        rt_pin_write(W5500_MOSI, (data & (1 << i)) ? PIN_HIGH : PIN_LOW);
        w5500_spi_delay();
        rt_pin_write(W5500_SCLK, PIN_HIGH);
        w5500_spi_delay();
    }
    rt_pin_write(W5500_SCLK, PIN_LOW);
}

static uint8_t w5500_spi_read(void)
{
    uint8_t data = 0;
    rt_pin_write(W5500_MOSI, PIN_HIGH);
    for (int i = 7; i >= 0; i--) {
        rt_pin_write(W5500_SCLK, PIN_LOW);
        w5500_spi_delay();
        rt_pin_write(W5500_SCLK, PIN_HIGH);
        w5500_spi_delay();
        if (rt_pin_read(W5500_MISO) == PIN_HIGH) data |= (1 << i);
    }
    rt_pin_write(W5500_SCLK, PIN_LOW);
    return data;
}

/**
 * Read a W5500 common register (block=0, read=0)
 * Uses WIZnet library 3-byte format:
 *   AddrSel = (offset << 8) | (block << 3) | (RWB << 2) | VDM_OP
 *   3-byte phase: [AddrSel>>16] [AddrSel>>8] [AddrSel&0xFF]
 */
static uint8_t w5500_read_reg(uint16_t addr)
{
    uint32_t addsel = ((uint32_t)addr << 8) | (0 << 3) | (0 << 2) | 0x00;
    uint8_t val;
    rt_pin_write(W5500_SCS, PIN_LOW);
    w5500_spi_delay();
    w5500_spi_write((uint8_t)((addsel >> 16) & 0xFF));
    w5500_spi_write((uint8_t)((addsel >> 8) & 0xFF));
    w5500_spi_write((uint8_t)(addsel & 0xFF));
    val = w5500_spi_read();                  /* read data */
    rt_pin_write(W5500_SCS, PIN_HIGH);
    w5500_spi_delay();
    return val;
}

/**
 * Slower SPI read for debugging timing issues (uses 3-byte WIZnet format)
 */
static uint8_t w5500_read_reg_slow(uint16_t addr)
{
    uint32_t addsel = ((uint32_t)addr << 8) | (0 << 3) | (0 << 2) | 0x00;
    uint8_t val = 0;
    int i, b;
    uint8_t phase[3] = {
        (uint8_t)((addsel >> 16) & 0xFF),
        (uint8_t)((addsel >> 8) & 0xFF),
        (uint8_t)(addsel & 0xFF)
    };

    rt_pin_write(W5500_SCS, PIN_LOW);
    rt_thread_mdelay(1);

    /* Send 3-byte address phase, MSB first */
    for (b = 0; b < 3; b++) {
        for (i = 7; i >= 0; i--) {
            rt_pin_write(W5500_SCLK, PIN_LOW);
            rt_pin_write(W5500_MOSI, (phase[b] & (1 << i)) ? PIN_HIGH : PIN_LOW);
            rt_thread_mdelay(1);
            rt_pin_write(W5500_SCLK, PIN_HIGH);
            rt_thread_mdelay(1);
        }
    }

    /* Read 8-bit data */
    rt_pin_write(W5500_MOSI, PIN_HIGH);
    for (i = 7; i >= 0; i--) {
        rt_pin_write(W5500_SCLK, PIN_LOW);
        rt_thread_mdelay(1);
        rt_pin_write(W5500_SCLK, PIN_HIGH);
        rt_thread_mdelay(1);
        if (rt_pin_read(W5500_MISO) == PIN_HIGH) val |= (1 << i);
    }

    rt_pin_write(W5500_SCLK, PIN_LOW);
    rt_pin_write(W5500_SCS, PIN_HIGH);
    return val;
}

static void cmd_w5500_test(int argc, char *argv[])
{
    uint8_t version;
    int pass = 0, fail = 0;

    rt_kprintf("\n========================================\n");
    rt_kprintf("  W5500 Hardware Diagnostic Test\n");
    rt_kprintf("========================================\n\n");

    /* 1. GPIO initialization */
    rt_kprintf("[1/5] GPIO Init...\n");
    rt_pin_mode(W5500_SCLK, PIN_MODE_OUTPUT);
    rt_pin_mode(W5500_MOSI, PIN_MODE_OUTPUT);
    rt_pin_mode(W5500_MISO, PIN_MODE_INPUT);
    rt_pin_mode(W5500_SCS,  PIN_MODE_OUTPUT);
    rt_pin_mode(W5500_RST,  PIN_MODE_OUTPUT);
    rt_pin_write(W5500_SCLK, PIN_LOW);
    rt_pin_write(W5500_MOSI, PIN_LOW);
    rt_pin_write(W5500_SCS,  PIN_HIGH);
    rt_pin_write(W5500_RST,  PIN_HIGH);

    rt_kprintf("  SCLK=PG5(out)  MOSI=PG1(out)\n");
    rt_kprintf("  MISO=PG3(in)   SCS =PG6(out)  RST=PG4(out)\n");
    rt_kprintf("  [OK] GPIO configured\n\n");

    /* 2. Raw MISO pin check */
    rt_kprintf("[2/6] MISO (PG3) Pin State Check...\n");
    {
        int reads = 10;
        int high = 0, low = 0;
        rt_pin_write(W5500_SCS, PIN_HIGH);
        for (int i = 0; i < reads; i++) {
            if (rt_pin_read(W5500_MISO) == PIN_HIGH) high++; else low++;
            rt_thread_mdelay(50);
        }
        rt_kprintf("  MISO (PG3): HIGH=%d times, LOW=%d times\n", high, low);
        if (high > 0 && low > 0)
            rt_kprintf("  [WARN] PG3 is FLOATING (no pull-up/down) -> W5500 may not be driving it\n");
        else if (high == reads)
            rt_kprintf("  PG3 stuck HIGH -> internal pull-up or external connection\n");
        else
            rt_kprintf("  PG3 stuck LOW -> may be shorted to GND or W5500 holding low\n");
    }
    rt_kprintf("\n");

    /* 3. Pin toggle test */
    rt_kprintf("[3/6] Pin Toggle Test...\n");
    rt_kprintf("  Toggling SCS (PG6) 3 times...\n");
    for (int i = 0; i < 3; i++) {
        rt_pin_write(W5500_SCS, PIN_LOW);
        rt_thread_mdelay(300);
        rt_pin_write(W5500_SCS, PIN_HIGH);
        rt_thread_mdelay(300);
    }
    rt_kprintf("  Toggling RST (PG4) LOW 100ms...\n");
    rt_pin_write(W5500_RST, PIN_LOW);
    rt_thread_mdelay(100);
    rt_pin_write(W5500_RST, PIN_HIGH);
    rt_thread_mdelay(50);
    rt_kprintf("\n");

    /* 4. Fast SPI Test */
    rt_kprintf("[4/6] Fast SPI (VERSIONR @ 0x0039)...\n");
    version = w5500_read_reg(0x0039);
    rt_kprintf("  VERSIONR = 0x%02X %s\n", version,
        version == 0x04 ? "[OK] W5500 DETECTED!" :
        version == 0xFF ? "[FAIL] 0xFF - MISO stuck HIGH?" :
        version == 0x00 ? "[FAIL] 0x00 - no response" :
        "[WARN] unexpected");
    rt_kprintf("\n");

    /* 5. Slow SPI Test */
    rt_kprintf("[5/6] SLOW SPI (1ms per bit, VERSIONR @ 0x0039)...\n");
    rt_kprintf("  (use multimeter to verify SCLK/MOSI toggling)\n");
    version = w5500_read_reg_slow(0x0039);
    rt_kprintf("  VERSIONR = 0x%02X %s\n", version,
        version == 0x04 ? "[OK] W5500 responding at slow speed!" :
        "[FAIL] Still no response at slow speed");
    rt_kprintf("\n");

    /* 6. Full register scan */
    rt_kprintf("[6/6] Register Scan...\n");
    struct { uint16_t addr; const char *name; } regs[] = {
        {0x0000, "MR       "},
        {0x0018, "IR       "},
        {0x002A, "PHYCFGR  "},
        {0x0039, "VERSIONR "},
    };
    for (int i = 0; i < 4; i++) {
        uint8_t val = w5500_read_reg(regs[i].addr);
        rt_kprintf("  %s (0x%04X) = 0x%02X\n", regs[i].name, regs[i].addr, val);
    }

    /* Summary */
    rt_kprintf("\n========================================\n");
    if (version == 0x04) {
        rt_kprintf("  STATUS: [OK] W5500 communication working!\n");
        rt_kprintf("  -> Run 'w5500 start' to bring up network\n");
    } else {
        rt_kprintf("  STATUS: [FAIL] W5500 not responding\n");
        rt_kprintf("\n  Troubleshooting checklist:\n");
        rt_kprintf("  1. W5500 VCC -> 5V (NOT 3.3V!)\n");
        rt_kprintf("  2. W5500 GND -> GND\n");
        rt_kprintf("  3. Check wires with multimeter continuity\n");
        rt_kprintf("  4. Verify pins match:\n");
        rt_kprintf("     SCLK=PG5  MOSI=PG1  MISO=PG3\n");
        rt_kprintf("     SCS=PG6   RST=PG4\n");
        rt_kprintf("  5. Try swapping MOSI/MISO if unsure\n");
    }
    rt_kprintf("========================================\n\n");
}
MSH_CMD_EXPORT_ALIAS(cmd_w5500_test, w5500_diag, W5500 hardware diagnostic test);

/* ── Pin walk test: toggle each W5500 GPIO one at a time (1s each) ── */
static void cmd_w5500_pins(int argc, char *argv[])
{
    struct { rt_base_t pin; const char *name; } pins[] = {
        {W5500_SCLK, "SCLK (PG5)"},
        {W5500_MOSI, "MOSI (PG1)"},
        {W5500_SCS,  "SCS  (PG6)"},
        {W5500_RST,  "RST  (PG4)"},
        {W5500_MISO, "MISO (PG3) [will read, not write]"},
    };
    int num = sizeof(pins) / sizeof(pins[0]);

    /* Init ALL as output low first */
    for (int i = 0; i < num; i++) {
        rt_pin_mode(pins[i].pin, PIN_MODE_OUTPUT);
        rt_pin_write(pins[i].pin, PIN_LOW);
    }

    rt_kprintf("\n=== W5500 Pin Walk Test ===\n");
    rt_kprintf("Each pin will go HIGH for 1 second.\n");
    rt_kprintf("Check with multimeter at the MODULE end of each wire.\n\n");

    for (int i = 0; i < num; i++) {
        rt_kprintf("[%d/%d] %s -> HIGH now (check module pin!)\n", i+1, num, pins[i].name);
        for (int j = 0; j < num; j++)
            rt_pin_write(pins[j].pin, (j == i) ? PIN_HIGH : PIN_LOW);
        rt_thread_mdelay(1000);
    }

    /* All LOW */
    for (int i = 0; i < num; i++)
        rt_pin_write(pins[i].pin, PIN_LOW);

    /* Read MISO */
    rt_pin_mode(W5500_MISO, PIN_MODE_INPUT);
    rt_kprintf("\nAll pins LOW.\n");
    rt_kprintf("MISO (PG3) reads: %s\n",
        rt_pin_read(W5500_MISO) == PIN_HIGH ? "HIGH" : "LOW");
    rt_kprintf("(If MISO reads LOW, PG3 GPIO is OK)\n");
    rt_kprintf("\nPin walk complete. Did each pin toggle at the module?\n\n");
}
MSH_CMD_EXPORT_ALIAS(cmd_w5500_pins, w5500_pins, walk W5500 pins for multimeter check);

/* ── Direct HAL register access W5500 test (bypass RT-Thread pin API) ── */
static void cmd_w5500_raw(int argc, char *argv[])
{
    rt_kprintf("\n=== W5500 RAW HAL Register Test ===\n\n");

    /* Ensure GPIOG clock is on */
    __HAL_RCC_GPIOG_CLK_ENABLE();

    /* Configure pins manually via HAL */
    GPIO_InitTypeDef gpio = {0};

    /* Outputs: PG1(MOSI), PG4(RST), PG5(SCLK), PG6(SCS) */
    gpio.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOG, &gpio);

    /* Input: PG3(MISO) */
    gpio.Pin = GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOG, &gpio);

    /* Initial states */
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_5, GPIO_PIN_RESET);  /* SCLK LOW */
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_1, GPIO_PIN_RESET);  /* MOSI LOW */
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_6, GPIO_PIN_SET);    /* SCS HIGH */
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_4, GPIO_PIN_SET);    /* RST HIGH */

    /* 1. Check MISO with CS HIGH */
    rt_kprintf("[1] MISO raw read (CS=HIGH): ");
    for (int i = 0; i < 10; i++) {
        rt_kprintf("%d", (GPIOG->IDR & GPIO_PIN_3) ? 1 : 0);
    }
    rt_kprintf("\n");

    /* 2. Reset W5500 */
    rt_kprintf("[2] Reset pulse...\n");
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_4, GPIO_PIN_RESET);
    rt_thread_mdelay(10);
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_4, GPIO_PIN_SET);
    rt_thread_mdelay(10);

    /* 3. Direct register SPI read using WIZnet 3-byte address format */
    rt_kprintf("[3] SPI read VERSIONR (WIZnet format)...\n");

    /* Library format: AddrSel = (offset<<8) | (block<<3) | (RWB<<2) | VDM
     * VERSIONR: offset=0x0039, block=0, read=0, VDM=0 → AddrSel=0x00003900
     * 3-byte phase: [0x00] [0x39] [0x00] */
    uint32_t addr = (0x0039 << 8) | (0 << 3) | (0 << 2) | 0x00;
    uint8_t phase[3] = {
        (uint8_t)((addr >> 16) & 0xFF),
        (uint8_t)((addr >> 8) & 0xFF),
        (uint8_t)(addr & 0xFF)
    };

    /* CS LOW */
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_6, GPIO_PIN_RESET);
    for (volatile int d = 0; d < 100; d++) __NOP();

    /* Send 3-byte address phase */
    for (int b = 0; b < 3; b++) {
        for (int i = 7; i >= 0; i--) {
            HAL_GPIO_WritePin(GPIOG, GPIO_PIN_5, GPIO_PIN_RESET);
            for (volatile int d = 0; d < 10; d++) __NOP();
            if (phase[b] & (1 << i))
                HAL_GPIO_WritePin(GPIOG, GPIO_PIN_1, GPIO_PIN_SET);
            else
                HAL_GPIO_WritePin(GPIOG, GPIO_PIN_1, GPIO_PIN_RESET);
            for (volatile int d = 0; d < 10; d++) __NOP();
            HAL_GPIO_WritePin(GPIOG, GPIO_PIN_5, GPIO_PIN_SET);
            for (volatile int d = 0; d < 10; d++) __NOP();
        }
    }
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_5, GPIO_PIN_RESET);

    /* Read 8-bit data */
    uint8_t val = 0;
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_1, GPIO_PIN_SET);
    for (int i = 7; i >= 0; i--) {
        HAL_GPIO_WritePin(GPIOG, GPIO_PIN_5, GPIO_PIN_RESET);
        for (volatile int d = 0; d < 10; d++) __NOP();
        HAL_GPIO_WritePin(GPIOG, GPIO_PIN_5, GPIO_PIN_SET);
        for (volatile int d = 0; d < 10; d++) __NOP();
        if (GPIOG->IDR & GPIO_PIN_3) val |= (1 << i);
    }
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_5, GPIO_PIN_RESET);

    /* CS HIGH */
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_6, GPIO_PIN_SET);

    rt_kprintf("  VERSIONR = 0x%02X %s\n", val,
        val == 0x04 ? "<< W5500 DETECTED! >>" : "(fail)");

    /* 4. Check GPIO IDR for all pins */
    rt_kprintf("[4] GPIOG IDR = 0x%04X\n", (unsigned)(GPIOG->IDR & 0xFFFF));
    rt_kprintf("    PG1(MOSI)=%d PG3(MISO)=%d PG4(RST)=%d PG5(SCLK)=%d PG6(SCS)=%d\n",
        !!(GPIOG->IDR & GPIO_PIN_1),
        !!(GPIOG->IDR & GPIO_PIN_3),
        !!(GPIOG->IDR & GPIO_PIN_4),
        !!(GPIOG->IDR & GPIO_PIN_5),
        !!(GPIOG->IDR & GPIO_PIN_6));

    rt_kprintf("\n=== Done ===\n\n");
}
MSH_CMD_EXPORT_ALIAS(cmd_w5500_raw, w5500_raw, W5500 raw HAL register test);

/* Swap MOSI/MISO test — try reading VERSIONR with pins swapped */
static void cmd_w5500_swap(int argc, char *argv[])
{
    uint8_t v_norm, v_swap;

    rt_kprintf("\n=== W5500 MOSI/MISO Swap Test ===\n\n");

    /* Init pins */
    rt_pin_mode(W5500_SCLK, PIN_MODE_OUTPUT);
    rt_pin_mode(W5500_SCS,  PIN_MODE_OUTPUT);
    rt_pin_mode(W5500_RST,  PIN_MODE_OUTPUT);
    rt_pin_write(W5500_SCLK, PIN_LOW);
    rt_pin_write(W5500_SCS,  PIN_HIGH);
    rt_pin_write(W5500_RST,  PIN_HIGH);

    /* Reset W5500 */
    rt_pin_write(W5500_RST, PIN_LOW);
    rt_thread_mdelay(10);
    rt_pin_write(W5500_RST, PIN_HIGH);
    rt_thread_mdelay(10);

    /* ── Test 1: Normal (PG1=MOSI, PG3=MISO) ── */
    rt_kprintf("[1] Normal: PG1->MOSI, PG3->MISO\n");
    rt_pin_mode(W5500_MOSI, PIN_MODE_OUTPUT);
    rt_pin_mode(W5500_MISO, PIN_MODE_INPUT);
    v_norm = w5500_read_reg(0x0039);
    rt_kprintf("    VERSIONR = 0x%02X %s\n", v_norm,
        v_norm == 0x04 ? "<< OK! >>" : "(fail)");

    /* ── Test 2: Swapped (PG1=MISO, PG3=MOSI) ── */
    rt_kprintf("[2] Swapped: PG1->MISO, PG3->MOSI\n");
    /* PG1 now reads, PG3 now writes */
    rt_pin_mode(W5500_MOSI, PIN_MODE_INPUT);   /* PG1 as input (MISO) */
    rt_pin_mode(W5500_MISO, PIN_MODE_OUTPUT);  /* PG3 as output (MOSI) */
    v_swap = w5500_read_reg(0x0039);
    rt_kprintf("    VERSIONR = 0x%02X %s\n", v_swap,
        v_swap == 0x04 ? "<< OK! >>" : "(fail)");

    /* ── Summary ── */
    rt_kprintf("\n--- Result ---\n");
    if (v_norm == 0x04)
        rt_kprintf("Wiring is CORRECT as-is.\n");
    else if (v_swap == 0x04)
        rt_kprintf("MOSI/MISO need to be SWAPPED! Change:\n"
                   "  PG1 -> MISO (module SO pin)\n"
                   "  PG3 -> MOSI (module SI pin)\n");
    else
        rt_kprintf("Both failed. Check:\n"
                   "  1. All wires with multimeter (continuity)\n"
                   "  2. Module 5V pin -> board 5V (measure with meter!)\n"
                   "  3. Module GND -> board GND\n"
                   "  4. Voltage between module VCC-GND (should be ~3.3V)\n");
    rt_kprintf("\n");
}
MSH_CMD_EXPORT_ALIAS(cmd_w5500_swap, w5500_swap, test W5500 with swapped MOSI/MISO);

#endif /* RT_USING_FINSH */
