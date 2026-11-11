/*
 * cloud_task.h - Cloud data upload (RW007 WiFi + OneNET MQTT)
 *
 * Hardware: SparkFire-1 (STM32F407ZGT6)
 */

#ifndef __CLOUD_TASK_H__
#define __CLOUD_TASK_H__

#include <rtthread.h>

/* Upload interval (milliseconds) */
#define CLOUD_UPLOAD_INTERVAL_MS  5000

/* Thread stack size */
#define CLOUD_THREAD_STACK_SIZE   4096

/* Thread priority (lower number = higher priority) */
#define CLOUD_THREAD_PRIORITY     16

/* Public API */
int  cloud_task_init(void);
void cloud_task_deinit(void);

#endif /* __CLOUD_TASK_H__ */
