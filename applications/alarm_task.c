/*
 * alarm_task.c - 环境报警任务 (蜂鸣器)
 *
 * 功能: 蜂鸣器 (PB0) 报警, 触发后响 10 秒自动关闭
 *
 * 报警条件:
 *   - 温度 > 30°C 或 PM2.5 > 70 ug/m3
 */

#include "alarm_task.h"
#include "sensor_task.h"
#include <rtdevice.h>
#include <board.h>

#define BUZZER_PIN  GET_PIN(B, 0)

#define DBG_TAG    "alarm"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

static rt_thread_t g_thread  = RT_NULL;
static rt_bool_t   g_running = RT_FALSE;
static rt_bool_t   g_active  = RT_FALSE;
static rt_bool_t   g_buzzer_done = RT_FALSE;
static rt_tick_t   g_buzzer_start = 0;

static rt_bool_t check_alarm(struct env_sensor_data *d)
{
    if (d->temp_valid && d->temperature > ALARM_TEMP_THRESHOLD) return RT_TRUE;
    if (d->pm_valid && d->pm2_5 > ALARM_PM25_THRESHOLD) return RT_TRUE;
    return RT_FALSE;
}

static void alarm_thread(void *parameter)
{
    struct env_sensor_data data;
    rt_bool_t first = RT_TRUE;

    rt_kprintf("[alarm] Buzzer alarm started (T>%dC or PM2.5>%d)\n",
               (int)ALARM_TEMP_THRESHOLD, ALARM_PM25_THRESHOLD);

    rt_pin_mode(BUZZER_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(BUZZER_PIN, PIN_LOW);

    while (g_running)
    {
        if (sensor_get_latest(&data) != RT_EOK)
        {
            if (first) { rt_kprintf("[alarm] Waiting for sensor...\n"); first = RT_FALSE; }
            rt_thread_mdelay(ALARM_CHECK_INTERVAL_MS);
            continue;
        }
        first = RT_FALSE;

        /* 每5秒打印状态摘要 */
        {
            static rt_tick_t lp = 0;
            rt_tick_t now = rt_tick_get();
            if (now - lp >= rt_tick_from_millisecond(5000))
            {
                lp = now;
                rt_kprintf("[alarm] T=%d.%d C  H=%d.%d %%  PM2.5=%d  alert=%d\n",
                           (int)data.temperature, (int)(data.temperature*10)%10,
                           (int)data.humidity, (int)(data.humidity*10)%10,
                           data.pm2_5, g_active);
            }
        }

        if (check_alarm(&data))
        {
            if (!g_active) {
                g_active = RT_TRUE;
                g_buzzer_done = RT_FALSE;
                g_buzzer_start = rt_tick_get();
                rt_pin_write(BUZZER_PIN, PIN_HIGH);
                rt_kprintf("\n*** ALARM! T=%d.%d PM2.5=%d — BUZZER ON ***\n\n",
                           (int)data.temperature, (int)(data.temperature*10)%10, data.pm2_5);
            } else {
                if (!g_buzzer_done) {
                    if (rt_tick_get() - g_buzzer_start >= rt_tick_from_millisecond(BUZZER_DURATION_MS)) {
                        rt_pin_write(BUZZER_PIN, PIN_LOW);
                        g_buzzer_done = RT_TRUE;
                        rt_kprintf("[alarm] Buzzer timeout -> OFF\n");
                    }
                }
            }
        }
        else
        {
            if (g_active) {
                rt_kprintf("[alarm] Conditions cleared, normal\n");
                g_active = RT_FALSE;
                g_buzzer_done = RT_FALSE;
                g_buzzer_start = 0;
                rt_pin_write(BUZZER_PIN, PIN_LOW);
            }
        }
        rt_thread_mdelay(ALARM_CHECK_INTERVAL_MS);
    }
}

int alarm_task_init(void)
{
    if (g_running) return RT_EOK;
    g_active = RT_FALSE;
    g_buzzer_done = RT_FALSE;
    g_buzzer_start = 0;
    g_running = RT_TRUE;
    g_thread = rt_thread_create("alarm", alarm_thread, RT_NULL,
                                ALARM_THREAD_STACK_SIZE, ALARM_THREAD_PRIORITY, 10);
    if (!g_thread) { g_running = RT_FALSE; return -RT_ERROR; }
    rt_thread_startup(g_thread);
    return RT_EOK;
}

void alarm_task_deinit(void)
{
    g_running = RT_FALSE;
    if (g_thread) { rt_thread_delete(g_thread); g_thread = RT_NULL; }
    rt_pin_write(BUZZER_PIN, PIN_LOW);
}

rt_bool_t alarm_is_active(void) { return g_active; }

#ifdef RT_USING_FINSH
#include <finsh.h>

static void cmd_buzzer(int argc, char *argv[])
{
    if (argc < 2) { rt_kprintf("Usage: buzzer <on|off>\n"); return; }
    if (rt_strcmp(argv[1], "on") == 0) {
        rt_pin_mode(BUZZER_PIN, PIN_MODE_OUTPUT);
        rt_pin_write(BUZZER_PIN, PIN_HIGH);
        rt_kprintf("Buzzer ON\n");
    } else if (rt_strcmp(argv[1], "off") == 0) {
        rt_pin_write(BUZZER_PIN, PIN_LOW);
        rt_kprintf("Buzzer OFF\n");
    }
}
MSH_CMD_EXPORT_ALIAS(cmd_buzzer, buzzer, buzzer <on|off>);

static void cmd_alarm_status(int argc, char *argv[])
{
    struct env_sensor_data data;
    rt_kprintf("\n========== Alarm Status ==========\n");
    rt_kprintf(" Active:  %s\n", g_active ? "YES" : "no");
    rt_kprintf(" Buzzer:  %s\n", g_buzzer_done ? "done" : "active");
    if (sensor_get_latest(&data) == RT_EOK) {
        rt_kprintf(" T=%d.%d C  H=%d.%d %%  PM2.5=%d\n",
                   (int)data.temperature, (int)(data.temperature*10)%10,
                   (int)data.humidity, (int)(data.humidity*10)%10, data.pm2_5);
    }
    rt_kprintf("==================================\n\n");
}
MSH_CMD_EXPORT_ALIAS(cmd_alarm_status, alarm_status, show alarm status);

#endif
