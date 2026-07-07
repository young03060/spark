/*
 * sensor_task.h - Sensor data acquisition task
 *
 * AHT10 temperature + humidity (I2C)
 * PMS5003 raw frame parser (UART2, 32-byte frames)
 * Thread-safe data access via mutex
 */

#ifndef __SENSOR_TASK_H__
#define __SENSOR_TASK_H__

#include <rtthread.h>

/* Air quality levels (used by LCD display) */
enum air_quality_level
{
    AIR_QUALITY_EXCELLENT = 0,
    AIR_QUALITY_GOOD,
    AIR_QUALITY_LIGHT,
    AIR_QUALITY_MODERATE,
    AIR_QUALITY_HEAVY,
    AIR_QUALITY_SEVERE,
};

/* PMS5003 raw data (one 32-byte frame) */
struct pms5003_data
{
    /* CF=1 standard particle concentration (μg/m³) */
    rt_uint16_t pm1_0_cf;
    rt_uint16_t pm2_5_cf;
    rt_uint16_t pm10_cf;

    /* Atmospheric environment concentration (μg/m³) */
    rt_uint16_t pm1_0_atm;
    rt_uint16_t pm2_5_atm;
    rt_uint16_t pm10_atm;

    /* Particle count per 0.1L */
    rt_uint16_t n_0_3um;
    rt_uint16_t n_0_5um;
    rt_uint16_t n_1_0um;
    rt_uint16_t n_2_5um;
    rt_uint16_t n_5_0um;
    rt_uint16_t n_10um;

    rt_bool_t  valid;        /* frame checksum OK */
};

/* Combined sensor data */
struct env_sensor_data
{
    /* Temperature & Humidity (AHT10) */
    float       temperature;
    float       humidity;

    /* PM2.5 (legacy, kept for compatibility) */
    rt_uint16_t pm2_5;
    rt_uint16_t pm10;

    /* PMS5003 detailed data */
    struct pms5003_data pms;

    /* Derived info */
    enum air_quality_level quality;
    rt_bool_t  temp_valid;
    rt_bool_t  humi_valid;
    rt_bool_t  pm_valid;

    /* Timestamp */
    rt_tick_t  timestamp;
};

/* Quality string helper */
const char *quality_string(enum air_quality_level level);

/* Public API */
int  sensor_task_init(void);
void sensor_task_deinit(void);
int  sensor_get_latest(struct env_sensor_data *data);

#endif /* __SENSOR_TASK_H__ */
