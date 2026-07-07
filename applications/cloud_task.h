/*
 * cloud_task.h - 云平台数据上传任务 (预留)
 *
 * 后期将通过 W5500 以太网模块 + OneNET MQTT 实现
 */

#ifndef __CLOUD_TASK_H__
#define __CLOUD_TASK_H__

#include <rtthread.h>

/* 云上传间隔 (毫秒) */
#define CLOUD_UPLOAD_INTERVAL_MS  5000

/* 云任务栈大小 */
#define CLOUD_THREAD_STACK_SIZE   4096

/* 云任务优先级 */
#define CLOUD_THREAD_PRIORITY     16

/* 公共 API */
int  cloud_task_init(void);
void cloud_task_deinit(void);

#endif /* __CLOUD_TASK_H__ */
