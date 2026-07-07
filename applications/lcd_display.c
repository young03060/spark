/*
 * lcd_display.c - LCD Display Manager for SparkFire Env Monitor
 *
 * Screen: 1.3" 240x240 ST7789V3 (8080 FSMC)
 * Single-page compact layout showing all sensor data
 */

#include "lcd_display.h"
#include <stdio.h>
#include <string.h>
#include <board.h>

#define DBG_TAG    "lcd"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

extern void lcd_clear(rt_uint16_t color);
extern void lcd_set_color(rt_uint16_t back, rt_uint16_t fore);
extern rt_err_t lcd_show_string(rt_uint16_t x, rt_uint16_t y,
                                rt_uint32_t size, const char *fmt, ...);
extern void lcd_draw_line(rt_uint16_t x0, rt_uint16_t y0,
                          rt_uint16_t x1, rt_uint16_t y1);
extern void lcd_fill(rt_uint16_t xs, rt_uint16_t ys,
                     rt_uint16_t xe, rt_uint16_t ye, rt_uint16_t color);

static char g_disp_buf[64];

/* Font: only 16 supported for dense layout; 24 for key values */
#define F16  16
#define F24  24

#define COLOR_DEEP_BLUE   0x01AF
#define COLOR_DIM_GRAY    0x632C

static rt_uint16_t pm25_color(rt_uint16_t v)
{
    if (v <= 35) return LCD_COLOR_GREEN;
    if (v <= 75) return LCD_COLOR_YELLOW;
    return LCD_COLOR_RED;
}

static const char *aq_text(rt_uint16_t pm2_5)
{
    if (pm2_5 <= 35) return "EXCELLENT";
    if (pm2_5 <= 75) return "GOOD";
    if (pm2_5 <= 115) return "LIGHT";
    if (pm2_5 <= 150) return "MODERATE";
    if (pm2_5 <= 250) return "HEAVY";
    return "SEVERE";
}

static void draw_all(struct env_sensor_data *data)
{
    /* ── Title bar (y:0-17) ── */
    lcd_fill(0, 0, 239, 17, COLOR_DEEP_BLUE);
    lcd_set_color(COLOR_DEEP_BLUE, LCD_COLOR_WHITE);
    lcd_show_string(30, 2, F16, "SPARKFIRE ENV MONITOR");

    /* ── Sensor status dots (top-right corner) ── */
    {
        /* AHT status dot */
        rt_uint16_t aht_color = data->temp_valid ? LCD_COLOR_GREEN : LCD_COLOR_RED;
        lcd_fill(222, 2, 228, 8, aht_color);
        /* PMS status dot */
        rt_uint16_t pms_color = data->pm_valid ? LCD_COLOR_GREEN : LCD_COLOR_RED;
        lcd_fill(232, 2, 238, 8, pms_color);
    }

    /* ── Row 1: TEMP + HUMI (labels y:20-36, values y:40-64) ── */
    lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GRAY);
    lcd_show_string(5, 20, F16, "TEMP");
    lcd_show_string(125, 20, F16, "HUMI");

    if (data->temp_valid) {
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_YELLOW);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d.%d C",
                    (int)data->temperature, (int)(data->temperature * 10) % 10);
    } else {
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GRAY);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "--.- C");
    }
    lcd_show_string(5, 40, F24, "%s", g_disp_buf);

    if (data->humi_valid) {
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_CYAN);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d.%d %%",
                    (int)data->humidity, (int)(data->humidity * 10) % 10);
    } else {
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GRAY);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "--.- %%");
    }
    lcd_show_string(125, 40, F24, "%s", g_disp_buf);

    /* divider (y:68) */
    lcd_set_color(LCD_COLOR_BLACK, COLOR_DIM_GRAY);
    lcd_draw_line(5, 68, 234, 68);

    /* ── Row 2: PM2.5 main + AQ badge (label y:72, value y:92-116, AQ at y:95) ── */
    lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GRAY);
    lcd_show_string(5, 72, F16, "PM2.5");

    if (data->pm_valid) {
        lcd_set_color(LCD_COLOR_BLACK, pm25_color(data->pm2_5));
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d ug/m3", data->pm2_5);
    } else {
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GRAY);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "--- ug/m3");
    }
    lcd_show_string(5, 92, F24, "%s", g_disp_buf);

    /* AQ badge (right side of PM2.5 value, same row) */
    if (data->pm_valid) {
        const char *aq = aq_text(data->pm2_5);
        rt_uint16_t aq_c = pm25_color(data->pm2_5);
        lcd_set_color(LCD_COLOR_BLACK, aq_c);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "[%s]", aq);
        lcd_show_string(130, 95, F16, "%s", g_disp_buf);
    }

    /* Color legend bar (y:120-130, now below PM2.5 value end at ~116) */
    {
        int ly = 120;
        /* Green box + label */
        lcd_fill(5, ly, 18, ly + 10, LCD_COLOR_GREEN);
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GREEN);
        lcd_show_string(25, ly - 2, F16, "0-35");

        /* Yellow box + label */
        lcd_fill(80, ly, 93, ly + 10, LCD_COLOR_YELLOW);
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_YELLOW);
        lcd_show_string(98, ly - 2, F16, "36-75");

        /* Red box + label */
        lcd_fill(160, ly, 173, ly + 10, LCD_COLOR_RED);
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_RED);
        lcd_show_string(178, ly - 2, F16, ">75");
    }

    /* divider (y:134) */
    lcd_set_color(LCD_COLOR_BLACK, COLOR_DIM_GRAY);
    lcd_draw_line(5, 134, 234, 134);

    /* ── Row 3: PM1.0/PM2.5/PM10 CF values (labels y:138, values y:158) ── */
    lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GRAY);
    lcd_show_string(10, 138, F16, "PM1.0");
    lcd_show_string(90, 138, F16, "PM2.5");
    lcd_show_string(170, 138, F16, "PM10");

    if (data->pm_valid) {
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GREEN);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d", data->pms.pm1_0_cf);
        lcd_show_string(10, 158, F16, "%s", g_disp_buf);

        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_YELLOW);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d", data->pms.pm2_5_cf);
        lcd_show_string(90, 158, F16, "%s", g_disp_buf);

        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_ORANGE);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d", data->pms.pm10_cf);
        lcd_show_string(170, 158, F16, "%s", g_disp_buf);
    } else {
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GRAY);
        lcd_show_string(10, 158, F16, "--");
        lcd_show_string(90, 158, F16, "--");
        lcd_show_string(170, 158, F16, "--");
    }

    /* ── Row 4: Particle counts (compact inline, y:178-220) ── */
    lcd_set_color(LCD_COLOR_BLACK, COLOR_DIM_GRAY);
    lcd_draw_line(5, 178, 234, 178);

    lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GRAY);
    lcd_show_string(45, 182, F16, "Particles /0.1L");

    if (data->pm_valid) {
        /* Row A: >0.3  >0.5  >1.0  (y:200) */
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GRAY);
        lcd_show_string(8, 200, F16, ">0.3");
        lcd_show_string(86, 200, F16, ">0.5");
        lcd_show_string(164, 200, F16, ">1.0");
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_CYAN);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d", data->pms.n_0_3um);
        lcd_show_string(45, 200, F16, "%s", g_disp_buf);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d", data->pms.n_0_5um);
        lcd_show_string(123, 200, F16, "%s", g_disp_buf);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d", data->pms.n_1_0um);
        lcd_show_string(201, 200, F16, "%s", g_disp_buf);

        /* Row B: >2.5  >5.0  >10  (y:220) */
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GRAY);
        lcd_show_string(8, 220, F16, ">2.5");
        lcd_show_string(86, 220, F16, ">5.0");
        lcd_show_string(164, 220, F16, ">10");
        lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_CYAN);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d", data->pms.n_2_5um);
        lcd_show_string(45, 220, F16, "%s", g_disp_buf);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d", data->pms.n_5_0um);
        lcd_show_string(123, 220, F16, "%s", g_disp_buf);
        rt_snprintf(g_disp_buf, sizeof(g_disp_buf), "%d", data->pms.n_10um);
        lcd_show_string(205, 220, F16, "%s", g_disp_buf);
    }
}

/* ══════════════════════════════════════════════════════════════ */

int lcd_display_init(void)
{
    lcd_clear(LCD_COLOR_BLACK);
    LOG_I("LCD initialized (single-page mode)");
    return RT_EOK;
}

void lcd_display_show_logo(void)
{
    lcd_clear(LCD_COLOR_BLACK);
    lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_CYAN);
    lcd_show_string(42, 90, 24, "SparkFire");
    lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_WHITE);
    lcd_show_string(45, 120, 16, "Env Monitor");
    lcd_set_color(LCD_COLOR_BLACK, LCD_COLOR_GRAY);
    lcd_show_string(35, 145, 16, "Starting up...");
}

void lcd_display_update(struct env_sensor_data *data)
{
    if (data == RT_NULL) return;
    draw_all(data);
}

void lcd_clear_screen(rt_uint16_t color)
{
    lcd_clear(color);
}
