/*
 * cloud_task.c - 云平台数据上传任务
 *
 * 当前通过串口输出结构化 CSV 数据，后期可通过 W5500 以太网
 * 模块升级为 OneNET MQTT 云平台数据上传。
 *
 * OneNET 设备信息:
 *   产品 ID: 2gjg8wq19N
 *   设备名称: spark1
 *
 * 硬件平台: 星火一号 (STM32F407ZGT6)
 */

#include "cloud_task.h"
#include "sensor_task.h"
#include <stdio.h>
#include <string.h>

#define DBG_TAG    "cloud"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

/* ── 内部状态 ── */
static rt_thread_t  g_cloud_thread   = RT_NULL;
static rt_bool_t    g_cloud_running  = RT_FALSE;
static rt_uint32_t  g_upload_count   = 0;
static rt_uint32_t  g_upload_errors  = 0;

/* ── 报警状态查询 (外部符号) ── */
extern rt_bool_t alarm_is_active(void);

/*
 * CSV 数据格式 (一条完整记录):
 * CLOUD:<tick>,<temp*10>,<humi*10>,<pm1_0>,<pm2_5>,<pm10>,
 *       <pm1_0_atm>,<pm2_5_atm>,<pm10_atm>,
 *       <n0_3>,<n0_5>,<n1_0>,<n2_5>,<n5_0>,<n10>,
 *       <t_valid>,<h_valid>,<p_valid>,<alarm>,<seq>
 *
 * 字段: 共 20 个字段，逗号分隔，无空格
 *   tick       - RT-Thread 系统 tick
 *   temp*10    - 温度 ×10 (整数, 如 253 = 25.3°C)
 *   humi*10    - 湿度 ×10 (整数, 如 585 = 58.5%)
 *   pm1_0      - PM1.0 CF=1 (μg/m³)
 *   pm2_5      - PM2.5 CF=1 (μg/m³)
 *   pm10       - PM10 CF=1 (μg/m³)
 *   pm1_0_atm  - PM1.0 大气环境 (μg/m³)
 *   pm2_5_atm  - PM2.5 大气环境 (μg/m³)
 *   pm10_atm   - PM10 大气环境 (μg/m³)
 *   n0_3       - >0.3μm 粒子数 /0.1L
 *   n0_5       - >0.5μm 粒子数 /0.1L
 *   n1_0       - >1.0μm 粒子数 /0.1L
 *   n2_5       - >2.5μm 粒子数 /0.1L
 *   n5_0       - >5.0μm 粒子数 /0.1L
 *   n10        - >10μm  粒子数 /0.1L
 *   t_valid    - 温度有效标志 (0/1)
 *   h_valid    - 湿度有效标志 (0/1)
 *   p_valid    - PM 有效标志 (0/1)
 *   alarm      - 报警状态 (0/1)
 *   seq        - 上传序号 (递增)
 */

/* ── 输出 CSV 头部 (启动时调用一次) ── */
static void cloud_print_header(void)
{
    rt_kprintf("\n");
    rt_kprintf("================================================\n");
    rt_kprintf("CLOUD_UPLOAD: SparkFire Env Monitor v1.1\n");
    rt_kprintf("CLOUD_FORMAT: CSV (20 fields)\n");
    rt_kprintf("CLOUD_COLS: tick,temp_x10,humi_x10,"
               "pm1_0_cf,pm2_5_cf,pm10_cf,"
               "pm1_0_atm,pm2_5_atm,pm10_atm,"
               "n0_3,n0_5,n1_0,n2_5,n5_0,n10,"
               "t_valid,h_valid,p_valid,alarm,seq\n");
    rt_kprintf("================================================\n\n");
}

/* ── 主线程 ── */
static void cloud_thread_entry(void *parameter)
{
    struct env_sensor_data data;
    rt_bool_t header_printed = RT_FALSE;

    LOG_I("Cloud upload thread started (serial CSV mode)");
    LOG_I("Upload interval: %d ms", CLOUD_UPLOAD_INTERVAL_MS);

    while (g_cloud_running)
    {
        /* 获取最新传感器数据 */
        if (sensor_get_latest(&data) != RT_EOK)
        {
            g_upload_errors++;
            rt_thread_mdelay(CLOUD_UPLOAD_INTERVAL_MS);
            continue;
        }

        /* 首次成功获取数据时打印头部 */
        if (!header_printed)
        {
            cloud_print_header();
            header_printed = RT_TRUE;
        }

        g_upload_count++;

        /* 构建并输出 CSV 数据行 */
        rt_kprintf("CLOUD:%lu,%d,%d,"
                   "%u,%u,%u,"
                   "%u,%u,%u,"
                   "%u,%u,%u,%u,%u,%u,"
                   "%d,%d,%d,%d,%lu\n",

            /* tick */
            (unsigned long)data.timestamp,

            /* temp*10, humi*10 */
            (int)(data.temperature * 10.0f),
            (int)(data.humidity * 10.0f),

            /* PM1.0/2.5/10 CF=1 */
            (unsigned int)data.pms.pm1_0_cf,
            (unsigned int)data.pms.pm2_5_cf,
            (unsigned int)data.pms.pm10_cf,

            /* PM1.0/2.5/10 atmospheric */
            (unsigned int)data.pms.pm1_0_atm,
            (unsigned int)data.pms.pm2_5_atm,
            (unsigned int)data.pms.pm10_atm,

            /* Particle counts /0.1L */
            (unsigned int)data.pms.n_0_3um,
            (unsigned int)data.pms.n_0_5um,
            (unsigned int)data.pms.n_1_0um,
            (unsigned int)data.pms.n_2_5um,
            (unsigned int)data.pms.n_5_0um,
            (unsigned int)data.pms.n_10um,

            /* Validity flags */
            data.temp_valid ? 1 : 0,
            data.humi_valid ? 1 : 0,
            data.pm_valid   ? 1 : 0,

            /* Alarm status */
            alarm_is_active() ? 1 : 0,

            /* Sequence number */
            (unsigned long)g_upload_count);

        rt_thread_mdelay(CLOUD_UPLOAD_INTERVAL_MS);
    }
}

/* ── 公共 API ── */

int cloud_task_init(void)
{
    if (g_cloud_running) return RT_EOK;

    g_cloud_running = RT_TRUE;
    g_upload_count = 0;
    g_upload_errors = 0;

    g_cloud_thread = rt_thread_create(
        "cloud", cloud_thread_entry, RT_NULL,
        CLOUD_THREAD_STACK_SIZE, CLOUD_THREAD_PRIORITY, 10);

    if (!g_cloud_thread)
    {
        g_cloud_running = RT_FALSE;
        LOG_E("Failed to create cloud thread!");
        return -RT_ERROR;
    }

    rt_thread_startup(g_cloud_thread);
    return RT_EOK;
}

void cloud_task_deinit(void)
{
    g_cloud_running = RT_FALSE;
    if (g_cloud_thread)
    {
        rt_thread_delete(g_cloud_thread);
        g_cloud_thread = RT_NULL;
    }
    LOG_I("Cloud upload stopped. Sent: %lu, Errors: %lu",
          g_upload_count, g_upload_errors);
}

/* ── Finsh 调试命令 ── */
#ifdef RT_USING_FINSH
#include <finsh.h>

static void cmd_cloud_status(int argc, char *argv[])
{
    rt_kprintf("\n==== Cloud Upload Status ====\n");
    rt_kprintf(" Running:  %s\n", g_cloud_running ? "YES" : "no");
    rt_kprintf(" Uploaded: %lu packets\n", g_upload_count);
    rt_kprintf(" Errors:   %lu\n", g_upload_errors);
    rt_kprintf(" Interval: %d ms\n", CLOUD_UPLOAD_INTERVAL_MS);
    rt_kprintf("============================\n\n");
}
MSH_CMD_EXPORT_ALIAS(cmd_cloud_status, cloud_status, show cloud upload status);

#endif /* RT_USING_FINSH */
