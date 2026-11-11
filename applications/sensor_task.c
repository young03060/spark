/*
 * sensor_task.c - Sensor data acquisition
 *
 * AHT10/AHT20/AHT21 temperature + humidity via I2C (tries I2C3 then I2C2)
 * PMS5003 is handled by PMSxx software package independently
 */

#include "sensor_task.h"
#include <rtdevice.h>
#include <board.h>
#include <string.h>

#define DBG_TAG    "sensor"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

/* I2C buses to probe (in order): I2C3=PE0/PE1, I2C2=PF1/PF0 */
static const char *aht_i2c_buses[] = {"i2c3", "i2c2"};
#define AHT10_I2C_ADDR     0x38
#define AHT10_I2C_ADDR_ALT 0x39

/* Sensor params */
#define SENSOR_REFRESH_MS  2000
#define SENSOR_THREAD_STACK 2048
#define SENSOR_THREAD_PRIO  15

/* ---- Shared data ---- */
static struct env_sensor_data g_latest_data;
static rt_mutex_t             g_data_mutex = RT_NULL;
static rt_thread_t            g_sensor_thread = RT_NULL;
static rt_bool_t              g_running = RT_FALSE;
static rt_bool_t              aht10_inited = RT_FALSE;
static rt_uint8_t             aht10_addr   = AHT10_I2C_ADDR;
static rt_device_t            aht_i2c_dev  = RT_NULL;  /* active I2C bus */
static rt_uint32_t            aht10_fail_count = 0;
static rt_uint32_t            aht10_ok_count   = 0;

/* ---- Quality string ---- */
const char *quality_string(enum air_quality_level level)
{
    switch (level) {
    case AIR_QUALITY_EXCELLENT: return "Excellent";
    case AIR_QUALITY_GOOD:      return "Good";
    case AIR_QUALITY_LIGHT:     return "LightPoll.";
    case AIR_QUALITY_MODERATE:  return "Moderate";
    case AIR_QUALITY_HEAVY:     return "Heavy";
    case AIR_QUALITY_SEVERE:    return "Severe";
    default:                     return "Unknown";
    }
}

static enum air_quality_level get_quality(rt_uint16_t pm2_5)
{
    if (pm2_5 <= 35)       return AIR_QUALITY_EXCELLENT;
    else if (pm2_5 <= 75)  return AIR_QUALITY_GOOD;
    else if (pm2_5 <= 115) return AIR_QUALITY_LIGHT;
    else if (pm2_5 <= 150) return AIR_QUALITY_MODERATE;
    else if (pm2_5 <= 250) return AIR_QUALITY_HEAVY;
    else                    return AIR_QUALITY_SEVERE;
}

/* ---- PMS5003 via raw UART2 frame parser ---- */
/*
 * PMS5003 32-byte data frame (9600-8N1):
 *   [0-1]   Start: 0x42 0x4D
 *   [2-3]   Frame len (0x001C = 28)
 *   [4-5]   PM1.0 CF=1  (μg/m³)
 *   [6-7]   PM2.5 CF=1
 *   [8-9]   PM10 CF=1
 *   [10-11] PM1.0 atm
 *   [12-13] PM2.5 atm
 *   [14-15] PM10 atm
 *   [16-17] >0.3μm /0.1L
 *   [18-19] >0.5μm /0.1L
 *   [20-21] >1.0μm /0.1L
 *   [22-23] >2.5μm /0.1L
 *   [24-25] >5.0μm /0.1L
 *   [26-27] >10μm  /0.1L
 *   [28-29] Reserved
 *   [30-31] Checksum (big-endian sum of bytes 0-29)
 */
#define PMS_FRAME_LEN     32
#define PMS_START1        0x42
#define PMS_START2        0x4D

static rt_device_t pms_uart_dev = RT_NULL;
static rt_uint8_t  pms_rx_buf[PMS_FRAME_LEN * 2];
static rt_uint8_t  pms_frame[PMS_FRAME_LEN];
static rt_uint32_t pms_frame_count = 0;
static rt_uint32_t pms_err_count   = 0;
static rt_uint32_t pms_rx_byte_count = 0;    /* total bytes received from UART2 */
static rt_tick_t   pms_last_data_tick = 0;   /* last time valid frame received */
#define PMS_DATA_TIMEOUT_MS  10000            /* warn if no PM data for 10 seconds */

static void pms_uart_init(void)
{
    if (pms_uart_dev != RT_NULL) return;

    pms_uart_dev = rt_device_find("uart2");
    if (pms_uart_dev == RT_NULL) {
        LOG_E("PMS: uart2 not found");
        return;
    }

    /* STEP 1: Open device FIRST (enables clocks + GPIO via HAL_UART_MspInit) */
    if (rt_device_open(pms_uart_dev, RT_DEVICE_FLAG_RDONLY | RT_DEVICE_FLAG_INT_RX) != RT_EOK) {
        LOG_E("PMS: failed to open uart2");
        pms_uart_dev = RT_NULL;
        return;
    }

    /* STEP 2: THEN configure baud rate (device must be open for config to apply) */
    struct serial_configure cfg = RT_SERIAL_CONFIG_DEFAULT;
    cfg.baud_rate = BAUD_RATE_9600;
    /* NOTE: do NOT change cfg.bufsz — if it differs from the driver's default
     * (RT_SERIAL_RB_BUFSZ=64) while the device is already open, rt_serial_control
     * rejects RT_DEVICE_CTRL_CONFIG with RT_EBUSY and the baud rate never applies. */
    {
        rt_err_t ctl_ret = rt_device_control(pms_uart_dev, RT_DEVICE_CTRL_CONFIG, &cfg);
        if (ctl_ret != RT_EOK)
            LOG_E("PMS: UART2 baud config failed (ret=%d)", ctl_ret);
    }

    pms_last_data_tick = rt_tick_get();
    LOG_I("PMS: raw UART2 reader started @ 9600 baud");
}

/**
 * Re-initialize UART2 for PMS5003 (called on data timeout)
 */
static void pms_uart_reinit(void)
{
    if (pms_uart_dev != RT_NULL) {
        rt_device_close(pms_uart_dev);
        pms_uart_dev = RT_NULL;
        LOG_W("PMS: UART2 closed for re-init");
    }
    rt_thread_mdelay(100);
    pms_uart_init();
}

static rt_bool_t pms_parse_frame(struct env_sensor_data *data)
{
    /* Read whatever is available from UART2 */
    int len = rt_device_read(pms_uart_dev, 0, pms_rx_buf, sizeof(pms_rx_buf));
    if (len <= 0) return RT_FALSE;
    pms_rx_byte_count += len;

    rt_bool_t updated = RT_FALSE;

    for (int i = 0; i < len; i++)
    {
        /* Slide window: shift buffer left, append new byte */
        memmove(pms_frame, pms_frame + 1, PMS_FRAME_LEN - 1);
        pms_frame[PMS_FRAME_LEN - 1] = pms_rx_buf[i];

        /* Check start bytes */
        if (pms_frame[0] != PMS_START1 || pms_frame[1] != PMS_START2)
            continue;

        /* Verify frame length */
        rt_uint16_t flen = ((rt_uint16_t)pms_frame[2] << 8) | pms_frame[3];
        if (flen != 28) continue;

        /* Verify checksum */
        rt_uint16_t sum = 0;
        for (int j = 0; j < PMS_FRAME_LEN - 2; j++)
            sum += pms_frame[j];
        rt_uint16_t cksum = ((rt_uint16_t)pms_frame[30] << 8) | pms_frame[31];
        if (sum != cksum) {
            pms_err_count++;
            continue;
        }

        /* ── Parse all fields (big-endian 16-bit) ── */
        #define BE16(off) (((rt_uint16_t)pms_frame[off] << 8) | pms_frame[(off)+1])

        data->pms.pm1_0_cf  = BE16(4);
        data->pms.pm2_5_cf  = BE16(6);
        data->pms.pm10_cf   = BE16(8);
        data->pms.pm1_0_atm = BE16(10);
        data->pms.pm2_5_atm = BE16(12);
        data->pms.pm10_atm  = BE16(14);
        data->pms.n_0_3um   = BE16(16);
        data->pms.n_0_5um   = BE16(18);
        data->pms.n_1_0um   = BE16(20);
        data->pms.n_2_5um   = BE16(22);
        data->pms.n_5_0um   = BE16(24);
        data->pms.n_10um    = BE16(26);
        data->pms.valid     = RT_TRUE;

        #undef BE16

        /* Fill legacy fields */
        data->pm2_5   = data->pms.pm2_5_cf;
        data->pm10    = data->pms.pm10_cf;
        data->pm_valid = RT_TRUE;
        data->quality  = get_quality(data->pm2_5);

        pms_frame_count++;
        pms_last_data_tick = rt_tick_get();
        updated = RT_TRUE;

        LOG_D("PMS: PM1.0=%d PM2.5=%d PM10=%d | "
              "0.3=%d 0.5=%d 1.0=%d 2.5=%d 5.0=%d 10=%d",
              data->pms.pm1_0_cf, data->pms.pm2_5_cf, data->pms.pm10_cf,
              data->pms.n_0_3um, data->pms.n_0_5um, data->pms.n_1_0um,
              data->pms.n_2_5um, data->pms.n_5_0um, data->pms.n_10um);
    }

    return updated;
}

static rt_bool_t read_pms_from_sensor(struct env_sensor_data *data)
{
    /* If UART2 is not open, try to re-init */
    if (pms_uart_dev == RT_NULL) {
        LOG_W("PMS: UART2 not open, attempting re-init...");
        pms_uart_reinit();
        return RT_FALSE;
    }

    /* If no data received for >10 seconds, try re-init */
    if (pms_last_data_tick > 0 &&
        rt_tick_get() - pms_last_data_tick > rt_tick_from_millisecond(PMS_DATA_TIMEOUT_MS)) {
        LOG_W("PMS: No data for %lu ms, re-initializing UART2...",
              (unsigned long)rt_tick_from_millisecond(PMS_DATA_TIMEOUT_MS) / RT_TICK_PER_SECOND * 1000);
        pms_uart_reinit();
        return RT_FALSE;
    }

    return pms_parse_frame(data);
}

/* ---- AHT10/AHT20 low-level I2C ---- */
/*
 * AHT10 vs AHT20 command sets:
 *   Command      | AHT10          | AHT20 (Adafruit-tested)
 *   -------------|----------------|------------------------
 *   Soft Reset   | 0xE1           | 0xBA
 *   Calibrate    | 0xBE 0x08 0x00 | 0xE1 0x08 0x00
 *   Trigger      | 0xAC 0x33 0x00 | 0xAC 0x33 0x00 (same)
 */

#define AHT_CMD_RESET_AHT20    0xBA
#define AHT_CMD_RESET_AHT10    0xE1
#define AHT_CMD_CAL_AHT20      0xE1
#define AHT_CMD_CAL_AHT10      0xBE
#define AHT_CMD_TRIGGER        0xAC
#define AHT_STATUS_BUSY        0x80
#define AHT_STATUS_CALIBRATED  0x08

static rt_uint8_t aht_read_status(rt_device_t i2c_dev)
{
    rt_uint8_t status = 0xFF;
    struct rt_i2c_msg msg;
    msg.addr  = aht10_addr;
    msg.flags = RT_I2C_RD;
    msg.buf   = &status;
    msg.len   = 1;
    rt_i2c_transfer((struct rt_i2c_bus_device *)i2c_dev, &msg, 1);
    return status;
}

static rt_err_t aht_wait_ready(rt_device_t i2c_dev)
{
    int timeout = 30;
    while (timeout-- > 0) {
        rt_uint8_t s = aht_read_status(i2c_dev);
        if (s != 0xFF && !(s & AHT_STATUS_BUSY))
            return RT_EOK;
        rt_thread_mdelay(10);
    }
    return -RT_ETIMEOUT;
}

/**
 * Initialize AHT10/AHT20/AHT21 — tries all bus + command combos
 */
static rt_err_t aht10_init(rt_device_t i2c_dev)
{
    rt_uint8_t cmd[3], status;
    struct rt_i2c_msg msgs[2];

    if (aht10_inited) return RT_EOK;

    rt_device_open(i2c_dev, RT_DEVICE_FLAG_RDWR);

    struct { rt_uint8_t addr; rt_uint8_t rst; rt_uint8_t cal; const char *desc; } combos[] = {
        { AHT10_I2C_ADDR,     AHT_CMD_RESET_AHT20, AHT_CMD_CAL_AHT20, "AHT20@0x38" },
        { AHT10_I2C_ADDR_ALT, AHT_CMD_RESET_AHT20, AHT_CMD_CAL_AHT20, "AHT20@0x39" },
        { AHT10_I2C_ADDR,     AHT_CMD_RESET_AHT10, AHT_CMD_CAL_AHT10, "AHT10@0x38" },
        { AHT10_I2C_ADDR_ALT, AHT_CMD_RESET_AHT10, AHT_CMD_CAL_AHT10, "AHT10@0x39" },
    };

    for (int i = 0; i < 4; i++) {
        /* Step 1: Soft Reset */
        cmd[0] = combos[i].rst;
        msgs[0].addr  = combos[i].addr;
        msgs[0].flags = RT_I2C_WR;
        msgs[0].buf   = cmd;
        msgs[0].len   = 1;
        if (rt_i2c_transfer((struct rt_i2c_bus_device *)i2c_dev, msgs, 1) != 1)
            continue;
        rt_thread_mdelay(20);

        /* Step 2: Wait ready */
        if (aht_wait_ready(i2c_dev) != RT_EOK) continue;

        /* Step 3: Read status */
        status = aht_read_status(i2c_dev);
        LOG_I("%s: status after reset=0x%02X (busy=%d cal=%d)",
              combos[i].desc, status, (status>>7)&1, (status>>3)&1);

        /* Step 4: Calibrate (may NACK on AHT20 — ignore) */
        cmd[0] = combos[i].cal;
        cmd[1] = 0x08; cmd[2] = 0x00;
        msgs[0].addr  = combos[i].addr;
        msgs[0].flags = RT_I2C_WR;
        msgs[0].buf   = cmd;
        msgs[0].len   = 3;
        rt_i2c_transfer((struct rt_i2c_bus_device *)i2c_dev, msgs, 1);

        rt_thread_mdelay(10);
        if (aht_wait_ready(i2c_dev) != RT_EOK) continue;

        /* Step 5: Verify calibrated */
        status = aht_read_status(i2c_dev);
        if (status != 0xFF && (status & AHT_STATUS_CALIBRATED)) {
            aht10_addr = combos[i].addr;
            aht10_inited = RT_TRUE;
            aht10_fail_count = 0;
            LOG_I("AHT sensor found: %s, OK", combos[i].desc);
            return RT_EOK;
        }
    }
    LOG_E("AHT init failed — no sensor responded.");
    return -RT_ERROR;
}

static rt_err_t aht10_read_raw(rt_device_t i2c_dev, float *temp, float *humi)
{
    rt_uint8_t cmd[3] = {AHT_CMD_TRIGGER, 0x33, 0x00};
    rt_uint8_t data[6] = {0};
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = aht10_addr;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = cmd;
    msgs[0].len   = 3;
    if (rt_i2c_transfer((struct rt_i2c_bus_device *)i2c_dev, msgs, 1) != 1)
        return -RT_ERROR;

    rt_thread_mdelay(80);

    msgs[0].flags = RT_I2C_RD;
    msgs[0].buf   = data;
    msgs[0].len   = 6;
    if (rt_i2c_transfer((struct rt_i2c_bus_device *)i2c_dev, msgs, 1) != 1)
        return -RT_ERROR;

    if (data[0] & AHT_STATUS_BUSY)
        return -RT_ERROR;

    rt_uint32_t raw_humi = ((rt_uint32_t)data[1] << 12) |
                           ((rt_uint32_t)data[2] << 4)  |
                           ((rt_uint32_t)data[3] >> 4);
    rt_uint32_t raw_temp = (((rt_uint32_t)data[3] & 0x0F) << 16) |
                           ((rt_uint32_t)data[4] << 8)  |
                           (rt_uint32_t)data[5];

    *humi = (float)raw_humi * 100.0f / 1048576.0f;
    *temp = (float)raw_temp * 200.0f / 1048576.0f - 50.0f;

    if (*temp < -40.0f || *temp > 85.0f || *humi < 0.0f || *humi > 100.0f)
        return -RT_ERROR;

    return RT_EOK;
}

/**
 * Read AHT — tries all configured I2C buses (i2c3, then i2c2)
 */
static rt_err_t aht10_read(float *temp, float *humi)
{
    rt_err_t ret;

    /* Find working I2C bus */
    if (aht_i2c_dev == RT_NULL) {
        for (int b = 0; b < sizeof(aht_i2c_buses)/sizeof(aht_i2c_buses[0]); b++) {
            aht_i2c_dev = rt_device_find(aht_i2c_buses[b]);
            if (aht_i2c_dev != RT_NULL) {
                LOG_I("AHT: using I2C bus '%s'", aht_i2c_buses[b]);
                break;
            }
        }
        if (aht_i2c_dev == RT_NULL) {
            LOG_E("No I2C bus found (tried i2c3, i2c2)");
            return -RT_ERROR;
        }
    }

    /* Re-init after 5 consecutive failures */
    if (aht10_inited && aht10_fail_count >= 5) {
        LOG_W("AHT: %lu consecutive failures, re-initializing...", aht10_fail_count);
        aht10_inited = RT_FALSE;
        /* Try next bus if this one keeps failing */
        aht_i2c_dev = RT_NULL;
    }

    if (!aht10_inited) {
        /* Try all buses for init */
        for (int b = 0; b < sizeof(aht_i2c_buses)/sizeof(aht_i2c_buses[0]); b++) {
            rt_device_t dev = rt_device_find(aht_i2c_buses[b]);
            if (dev == RT_NULL) continue;
            LOG_I("Probing AHT on bus '%s'...", aht_i2c_buses[b]);
            ret = aht10_init(dev);
            if (ret == RT_EOK) {
                aht_i2c_dev = dev;
                break;
            }
        }
        if (!aht10_inited) {
            aht10_fail_count++;
            return -RT_ERROR;
        }
    }

    ret = aht10_read_raw(aht_i2c_dev, temp, humi);
    if (ret == RT_EOK) {
        aht10_fail_count = 0;
        aht10_ok_count++;
    } else {
        aht10_fail_count++;
    }
    return ret;
}

/* ---- Sensor thread ---- */
static void sensor_thread_entry(void *parameter)
{
    struct env_sensor_data data;
    rt_err_t ret;

    LOG_I("Sensor thread started");
    while (g_running) {
        rt_memset(&data, 0, sizeof(data));
        data.timestamp = rt_tick_get();

        data.temp_valid = RT_FALSE;
        data.humi_valid = RT_FALSE;
        ret = aht10_read(&data.temperature, &data.humidity);
        if (ret == RT_EOK) {
            data.temp_valid = RT_TRUE;
            data.humi_valid = RT_TRUE;
            LOG_D("T=%d.%d C  H=%d.%d %%",
                  (int)data.temperature, (int)(data.temperature*10)%10,
                  (int)data.humidity, (int)(data.humidity*10)%10);
        }

        data.pm_valid = RT_FALSE;
        read_pms_from_sensor(&data);

        /* Periodic PM data watchdog (every ~12 seconds = 6 cycles) */
        {
            static int pm_watchdog = 0;
            pm_watchdog++;
            if (pm_watchdog >= 6) {
                pm_watchdog = 0;
                if (!data.pm_valid) {
                    LOG_W("PM data STALE — no valid PMS5003 frames (count=%lu err=%lu)",
                          pms_frame_count, pms_err_count);
                } else {
                    LOG_D("PM OK: PM2.5=%d, frames=%lu", data.pm2_5, pms_frame_count);
                }
            }
        }

        rt_mutex_take(g_data_mutex, RT_WAITING_FOREVER);
        rt_memcpy(&g_latest_data, &data, sizeof(struct env_sensor_data));
        rt_mutex_release(g_data_mutex);

        rt_thread_mdelay(SENSOR_REFRESH_MS);
    }
}

/* ---- Public API ---- */
int sensor_task_init(void)
{
    g_data_mutex = rt_mutex_create("sens_mtx", RT_IPC_FLAG_FIFO);
    if (g_data_mutex == RT_NULL) return -RT_ERROR;
    rt_memset(&g_latest_data, 0, sizeof(g_latest_data));

    /* Initialize raw PMS5003 UART reader */
    pms_uart_init();

    g_running = RT_TRUE;
    g_sensor_thread = rt_thread_create("sensor", sensor_thread_entry,
                                       RT_NULL, SENSOR_THREAD_STACK,
                                       SENSOR_THREAD_PRIO, 10);
    if (g_sensor_thread == RT_NULL) {
        rt_mutex_delete(g_data_mutex);
        return -RT_ERROR;
    }
    rt_thread_startup(g_sensor_thread);
    return RT_EOK;
}

void sensor_task_deinit(void)
{
    g_running = RT_FALSE;
    if (g_sensor_thread) { rt_thread_delete(g_sensor_thread); g_sensor_thread = RT_NULL; }
    if (g_data_mutex)     { rt_mutex_delete(g_data_mutex); g_data_mutex = RT_NULL; }
}

int sensor_get_latest(struct env_sensor_data *data)
{
    if (g_data_mutex == RT_NULL || data == RT_NULL) return -RT_ERROR;
    rt_mutex_take(g_data_mutex, RT_WAITING_FOREVER);
    rt_memcpy(data, &g_latest_data, sizeof(struct env_sensor_data));
    rt_mutex_release(g_data_mutex);
    return RT_EOK;
}

/* ---- Finsh diagnostic commands ---- */
#ifdef RT_USING_FINSH
#include <finsh.h>

static void cmd_aht10_test(int argc, char *argv[])
{
    rt_device_t i2c_dev;
    float temp, humi;
    rt_err_t ret;

    /* Try to find an I2C bus */
    i2c_dev = rt_device_find("i2c3");
    if (i2c_dev == RT_NULL) i2c_dev = rt_device_find("i2c2");
    if (i2c_dev == RT_NULL) {
        rt_kprintf("ERROR: No I2C bus found (i2c3/i2c2)\n");
        return;
    }
    rt_kprintf("I2C bus found: OK\n");

    if (argc >= 2 && rt_strcmp(argv[1], "1") == 0) {
        aht10_inited = RT_FALSE;
        aht10_fail_count = 0;
        rt_kprintf("Force re-init\n");
    }
    rt_kprintf("State: inited=%d addr=0x%02X ok=%lu fail=%lu\n",
               aht10_inited, aht10_addr, aht10_ok_count, aht10_fail_count);

    if (!aht10_inited) {
        rt_kprintf("Initializing...\n");
        /* Try both buses */
        const char *buses[] = {"i2c3", "i2c2"};
        for (int b = 0; b < 2; b++) {
            rt_device_t dev = rt_device_find(buses[b]);
            if (dev == RT_NULL) {
                rt_kprintf("  Bus '%s': not registered\n", buses[b]);
                continue;
            }
            rt_kprintf("  Bus '%s': probing...\n", buses[b]);
            ret = aht10_init(dev);
            if (ret == RT_EOK) {
                aht_i2c_dev = dev;
                break;
            }
            rt_kprintf("  Bus '%s': no response\n", buses[b]);
        }
        if (!aht10_inited) {
            rt_kprintf("INIT FAILED! Sensor not responding on any bus.\n");
            rt_kprintf("Check: 1) Sensor VCC/GND  2) I2C pull-ups  3) Board revision\n");
            return;
        }
    }

    rt_kprintf("Reading...\n");
    if (aht_i2c_dev == RT_NULL) aht_i2c_dev = i2c_dev;
    ret = aht10_read_raw(aht_i2c_dev, &temp, &humi);
    if (ret == RT_EOK)
        rt_kprintf("SUCCESS! T=%d.%d C  H=%d.%d %%\n",
                   (int)temp, (int)(temp*10)%10, (int)humi, (int)(humi*10)%10);
    else
        rt_kprintf("READ FAILED (ret=%d)\n", ret);
}
MSH_CMD_EXPORT_ALIAS(cmd_aht10_test, aht10_test, direct AHT sensor diagnostic test);

static void cmd_pms_status(int argc, char *argv[])
{
    rt_kprintf("\n==== PMS5003 UART2 Status ====\n");
    rt_kprintf(" UART device: %s\n", pms_uart_dev ? "OPEN" : "CLOSED/NULL");
    rt_kprintf(" Valid frames: %lu\n", pms_frame_count);
    rt_kprintf(" Bad frames:   %lu\n", pms_err_count);
    rt_kprintf(" RX bytes:     %lu\n", pms_rx_byte_count);
    rt_kprintf(" Last 32 raw bytes:\n");
    for (int i = 0; i < PMS_FRAME_LEN; i++) {
        rt_kprintf("%02X ", pms_frame[i]);
        if ((i & 0x0F) == 0x0F) rt_kprintf("\n");
    }
    rt_kprintf("\n");
    if (pms_last_data_tick > 0) {
        rt_tick_t elapsed = rt_tick_get() - pms_last_data_tick;
        rt_kprintf(" Last data:    %lu ms ago\n",
                   (unsigned long)elapsed * 1000 / RT_TICK_PER_SECOND);
    } else {
        rt_kprintf(" Last data:    never\n");
    }

    if (argc >= 2 && rt_strcmp(argv[1], "reinit") == 0) {
        rt_kprintf("\nRe-initializing UART2...\n");
        pms_uart_reinit();
        rt_kprintf("Done. UART device now: %s\n",
                   pms_uart_dev ? "OPEN" : "CLOSED/NULL");
    }
    rt_kprintf("==============================\n\n");
}
MSH_CMD_EXPORT_ALIAS(cmd_pms_status, pms_status, show PMS5003 UART2 status [reinit]);

/**
 * 波特率扫描：直接 HAL 轮询 PA3，逐个波特率尝试，找出能收到 0x42 0x4D 帧头的速率。
 * 用于排查 PMS5003 克隆模块波特率与代码 9600 不匹配的问题。
 */
static void cmd_pms_scan(int argc, char *argv[])
{
    UART_HandleTypeDef huart2;
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    const rt_uint32_t bauds[] = {2400, 4800, 9600, 19200, 38400, 57600, 115200};
    const char *names[] = {"2400", "4800", "9600", "19200", "38400", "57600", "115200"};
    int n = sizeof(bauds) / sizeof(bauds[0]);

    rt_kprintf("\n=== PMS5003 baud scan (direct HAL polling) ===\n");

    for (int b = 0; b < n; b++)
    {
        /* 时钟 + GPIO: PA2/PA3 = USART2 AF7 */
        __HAL_RCC_USART2_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        GPIO_InitStruct.Pin = GPIO_PIN_2 | GPIO_PIN_3;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_PULLUP;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        huart2.Instance = USART2;
        huart2.Init.BaudRate = bauds[b];
        huart2.Init.WordLength = UART_WORDLENGTH_8B;
        huart2.Init.StopBits = UART_STOPBITS_1;
        huart2.Init.Parity = UART_PARITY_NONE;
        huart2.Init.Mode = UART_MODE_RX;
        huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
        huart2.Init.OverSampling = UART_OVERSAMPLING_16;
        HAL_UART_Init(&huart2);

        rt_uint32_t total = 0, matches = 0, errors = 0;
        rt_uint8_t  first[8] = {0};
        rt_uint8_t  prev = 0;
        rt_tick_t   end = rt_tick_get() + rt_tick_from_millisecond(600);

        while (rt_tick_get() < end)
        {
            if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE))
            {
                rt_uint8_t ch = (rt_uint8_t)(huart2.Instance->DR & 0xFF);
                if (total < 8) first[total] = ch;
                if (prev == 0x42 && ch == 0x4D) matches++;
                prev = ch;
                total++;
            }
            if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_ORE | UART_FLAG_FE | UART_FLAG_NE))
            {
                __HAL_UART_CLEAR_FLAG(&huart2, UART_FLAG_ORE | UART_FLAG_FE | UART_FLAG_NE);
                errors++;
            }
        }

        rt_kprintf("%5s baud: %4lu B  err=%lu  42-4D=%lu  first:",
                   names[b], total, errors, matches);
        for (rt_uint32_t i = 0; i < (total < 8 ? total : 8); i++)
            rt_kprintf(" %02X", first[i]);
        rt_kprintf(" %s\n", matches > 0 ? "  <<< HIT" : "");

        HAL_UART_DeInit(&huart2);
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_2 | GPIO_PIN_3);
    }

    rt_kprintf("\nRestoring UART2 for sensor driver...\n");
    pms_uart_reinit();
    rt_kprintf("=== done ===\n\n");
}
MSH_CMD_EXPORT_ALIAS(cmd_pms_scan, pms_scan, scan PMS5003 baud rates to find frame start);

static void cmd_i2c_scan(int argc, char *argv[])
{
    const char *bus_name = "i2c3";
    if (argc >= 2) bus_name = argv[1];

    rt_device_t i2c_dev = rt_device_find(bus_name);
    if (i2c_dev == RT_NULL) {
        rt_kprintf("I2C bus '%s' not found\n", bus_name);
        return;
    }

    rt_kprintf("Scanning '%s'...\n", bus_name);
    int found = 0;
    for (int addr7 = 1; addr7 < 128; addr7++) {
        if ((addr7 & 0x0F) == 0)
            rt_kprintf("%02X: ", addr7 & 0xF0);

        rt_uint8_t dummy = 0;
        struct rt_i2c_msg msg;
        msg.addr  = addr7;
        msg.flags = RT_I2C_WR;
        msg.buf   = &dummy;
        msg.len   = 1;

        if (rt_i2c_transfer((struct rt_i2c_bus_device *)i2c_dev, &msg, 1) == 1) {
            rt_kprintf("%02X ", addr7);
            found++;
        } else {
            rt_kprintf("-- ");
        }
        if ((addr7 & 0x0F) == 0x0F)
            rt_kprintf("\n");
    }
    rt_kprintf("Found %d device(s)\n", found);
}
MSH_CMD_EXPORT_ALIAS(cmd_i2c_scan, i2c_scan, scan I2C bus for devices);

#endif /* RT_USING_FINSH */
