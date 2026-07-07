/*
 * lcd_display.h - LCD 显示界面管理头文件
 *
 * 屏幕: 1.3寸 240x240 ST7789V3 (8080 FSMC)
 * 使用 RT-Thread 板级 LCD 驱动 API
 */

#ifndef __LCD_DISPLAY_H__
#define __LCD_DISPLAY_H__

#include <rtthread.h>
#include "sensor_task.h"

/* 颜色定义 (RGB565) */
#define LCD_COLOR_WHITE         0xFFFF
#define LCD_COLOR_BLACK         0x0000
#define LCD_COLOR_BLUE          0x001F
#define LCD_COLOR_RED           0xF800
#define LCD_COLOR_GREEN         0x07E0
#define LCD_COLOR_YELLOW        0xFFE0
#define LCD_COLOR_CYAN          0x07FF
#define LCD_COLOR_GRAY          0x8430
#define LCD_COLOR_DARK_GREEN    0x03E0
#define LCD_COLOR_ORANGE        0xFD20

/* 显示模块对外 API */
int  lcd_display_init(void);
void lcd_display_update(struct env_sensor_data *data);
void lcd_display_show_logo(void);
void lcd_clear_screen(rt_uint16_t color);

#endif /* __LCD_DISPLAY_H__ */
