/*
 * alarm_task.h - 环境报警任务 (蜂鸣器)
 *
 * 报警条件 (满足任一即触发):
 *   - 温度 > 30°C
 *   - PM2.5 > 70 ug/m3
 *
 * 硬件平台: 星火一号 (STM32F407ZGT6)
 * 操作系统: RT-Thread
 */

#ifndef __ALARM_TASK_H__
#define __ALARM_TASK_H__

#include <rtthread.h>

#define ALARM_TEMP_THRESHOLD    30.0f
#define ALARM_PM25_THRESHOLD    70          /* 默认 PM2.5 报警阈值 (μg/m³) */
#define BUZZER_DURATION_MS      10000
#define ALARM_CHECK_INTERVAL_MS 500
#define ALARM_THREAD_STACK_SIZE 2048
#define ALARM_THREAD_PRIORITY   14

int  alarm_task_init(void);
void alarm_task_deinit(void);
rt_bool_t alarm_is_active(void);

/* 运行时 PM2.5 报警阈值 (对应物模型 alarm_th, 0-100 μg/m³) */
void alarm_set_pm_threshold(int pm);
int  alarm_get_pm_threshold(void);

#endif
