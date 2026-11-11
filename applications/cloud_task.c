/*
 * cloud_task.c - RW007 WiFi + OneNET MQTT cloud data upload (dual device)
 *
 * Uses onboard RW007 WiFi module (SPI2) to connect to OneNET Studio (新一代)
 * via MQTT, with TWO device connections:
 *   - device "dw"    : temperature (tem) + humidity (hum)
 *   - device "pm002" : PM2.5 (PM)
 *
 * The OneNET package only supports a single MQTT connection, so this file
 * drives paho-mqtt directly with two independent MQTTClient instances.
 *
 * Hardware: SparkFire-1 (STM32F407ZGT6)
 */

#include "cloud_task.h"
#include "cloud_config.h"
#include "sensor_task.h"
#include "alarm_task.h"
#include <stdio.h>
#include <string.h>

#include <wlan_mgnt.h>
#include <paho_mqtt.h>
#include <cJSON.h>
#include <board.h>

#ifdef BSP_USING_LED_MATRIX
#include <drv_matrix_led.h>
#endif

#define DBG_TAG    "cloud"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

/* WiFi and device credentials are configured in cloud_config.h. */

/* ── OneNET Studio MQTT ── */
#define MQTT_SERVER_URI "tcp://183.230.40.96:1883"
#define MQTT_BUF_SIZE   2048

/* device "dw" : temperature + humidity (+ 下行控制 LED1/buzzer1/alarm_th) */
#define DW_POST_TOPIC   "$sys/" MQTT_PROID "/" DW_DEVID "/thing/property/post"
#define DW_SET_TOPIC    "$sys/" MQTT_PROID "/" DW_DEVID "/thing/property/set"
#define DW_SET_REPLY    "$sys/" MQTT_PROID "/" DW_DEVID "/thing/property/set_reply"

/* 蜂鸣器 GPIO (与 alarm_task.c 一致: PB0) */
#define BUZZER_PIN      GET_PIN(B, 0)

/* device "pm002" : PM2.5 */
#define PM002_POST_TOPIC "$sys/" MQTT_PROID "/" PM002_DEVID "/thing/property/post"

/* ── Internal state ── */
static rt_thread_t  g_cloud_thread   = RT_NULL;
static rt_bool_t    g_cloud_running  = RT_FALSE;
static rt_uint32_t  g_upload_count   = 0;
static rt_uint32_t  g_upload_errors  = 0;
static rt_bool_t    g_wifi_ready     = RT_FALSE;
static rt_bool_t    g_mqtt_started   = RT_FALSE;
static rt_sem_t     g_net_sem        = RT_NULL;

static MQTTClient   g_dw_client;
static MQTTClient   g_pm002_client;
static rt_bool_t    g_dw_connected    = RT_FALSE;
static rt_bool_t    g_pm002_connected = RT_FALSE;

/* 下行控制消息回调 (OneNET 属性设置) */
static void dw_property_set_cb(MQTTClient *c, MessageData *md);

/* ── WiFi event callbacks ── */
static void wifi_ready_handler(int event, struct rt_wlan_buff *buff, void *parameter)
{
    (void)event; (void)buff; (void)parameter;
    LOG_I("WiFi ready: IP obtained");
    g_wifi_ready = RT_TRUE;
    rt_sem_release(g_net_sem);
}

static void wifi_disconnect_handler(int event, struct rt_wlan_buff *buff, void *parameter)
{
    (void)event; (void)buff; (void)parameter;
    LOG_W("WiFi disconnected");
    g_wifi_ready = RT_FALSE;
}

/* ── MQTT connection callbacks ── */
static void dw_online_cb(MQTTClient *c)
{
    (void)c;
    g_dw_connected = RT_TRUE;
    LOG_I("dw MQTT online (tem + hum)");
}

static void dw_offline_cb(MQTTClient *c)
{
    (void)c;
    g_dw_connected = RT_FALSE;
    LOG_W("dw MQTT offline");
}

static void pm002_online_cb(MQTTClient *c)
{
    (void)c;
    g_pm002_connected = RT_TRUE;
    LOG_I("pm002 MQTT online (PM)");
}

static void pm002_offline_cb(MQTTClient *c)
{
    (void)c;
    g_pm002_connected = RT_FALSE;
    LOG_W("pm002 MQTT offline");
}

/* ── One MQTT client setup ──
 * sub_topic: 若非 NULL，则连接后订阅该 topic 并用 msg_cb 处理收到的消息
 *            (用于 dw 设备接收下行控制) */
static int mqtt_client_init(MQTTClient *c, const char *devid, const char *auth,
                            void (*online_cb)(MQTTClient *),
                            void (*offline_cb)(MQTTClient *),
                            const char *sub_topic,
                            void (*msg_cb)(MQTTClient *, MessageData *))
{
    MQTTPacket_connectData condata = MQTTPacket_connectData_initializer;

    memset(c, 0, sizeof(*c));
    c->uri = MQTT_SERVER_URI;

    memcpy(&c->condata, &condata, sizeof(condata));
    c->condata.clientID.cstring     = (char *)devid;
    c->condata.keepAliveInterval    = 30;
    c->condata.cleansession         = 1;
    c->condata.username.cstring     = (char *)MQTT_PROID;
    c->condata.password.cstring     = (char *)auth;

    c->buf_size = c->readbuf_size = MQTT_BUF_SIZE;
    c->buf     = (unsigned char *)rt_calloc(1, MQTT_BUF_SIZE);
    c->readbuf = (unsigned char *)rt_calloc(1, MQTT_BUF_SIZE);
    if (!c->buf || !c->readbuf)
    {
        LOG_E("No memory for MQTT client buffer");
        return -RT_ENOMEM;
    }

    c->online_callback  = online_cb;
    c->offline_callback = offline_cb;

    /* 下行订阅：连接后 paho 会按 messageHandlers 自动 subscribe */
    if (sub_topic != RT_NULL)
    {
        c->messageHandlers[0].topicFilter = rt_strdup(sub_topic);
        c->messageHandlers[0].callback    = msg_cb;
        c->messageHandlers[0].qos         = QOS1;
    }

    if (paho_mqtt_start(c) != 0)
    {
        LOG_E("paho_mqtt_start(%s) failed", devid);
        return -RT_ERROR;
    }
    return RT_EOK;
}

/* ── Publish a JSON string to a topic (QoS0, non-blocking) ── */
static int mqtt_publish(MQTTClient *c, const char *topic, const char *json)
{
    MQTTMessage msg;

    msg.qos        = QOS0;
    msg.retained   = 0;
    msg.dup        = 0;
    msg.id         = 0;
    msg.payload    = (void *)json;
    msg.payloadlen = strlen(json);

    return MQTTPublish(c, topic, &msg);
}

/* ── 下行控制：执行器状态 ── */
static rt_bool_t g_led1_state   = RT_FALSE;
static rt_bool_t g_buzzer_state = RT_FALSE;

static void ctl_led1(rt_bool_t on)
{
    g_led1_state = on;
#ifdef BSP_USING_LED_MATRIX
    if (on)
    {
        led_matrix_fill_rgb(255, 255, 255);   /* 全白 = 灯开 */
        led_matrix_reflash();
    }
    else
    {
        led_matrix_clear();                   /* 熄灭 */
    }
    LOG_I("LED1 -> %s", on ? "ON" : "OFF");
#else
    LOG_W("LED1 not available (LED matrix disabled)");
#endif
}

static void ctl_buzzer(rt_bool_t on)
{
    g_buzzer_state = on;
    rt_pin_mode(BUZZER_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(BUZZER_PIN, on ? PIN_HIGH : PIN_LOW);
    LOG_I("buzzer1 -> %s", on ? "ON" : "OFF");
}

/* 把当前执行器状态上报回去，让平台/小程序能读到 */
static void dw_post_control_state(void)
{
    char json[256];
    if (!g_dw_connected) return;
    rt_snprintf(json, sizeof(json),
        "{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{"
        "\"LED1\":{\"value\":%s},"
        "\"buzzer1\":{\"value\":%s},"
        "\"alarm_th\":{\"value\":%d}}}",
        (unsigned long)(g_upload_count + 1),
        g_led1_state   ? "true" : "false",
        g_buzzer_state ? "true" : "false",
        alarm_get_pm_threshold());
    mqtt_publish(&g_dw_client, DW_POST_TOPIC, json);
}

/* 取属性值，兼容 {"value":xxx} 与直接 xxx 两种写法 */
static cJSON *unwrap_value(cJSON *item)
{
    if (item && cJSON_IsObject(item))
    {
        cJSON *v = cJSON_GetObjectItem(item, "value");
        if (v) return v;
    }
    return item;
}

/* ── OneNET 属性设置下行回调 ── */
static void dw_property_set_cb(MQTTClient *c, MessageData *md)
{
    char *payload = (char *)md->message->payload;
    int   plen    = md->message->payloadlen;
    char *json;
    char  msg_id[64] = "0";
    char  reply[256];
    cJSON *root, *params, *item, *v;

    if (!payload || plen <= 0)
        return;

    /* cJSON 需要以 \0 结尾，先拷贝一份 */
    json = rt_malloc(plen + 1);
    if (!json) return;
    memcpy(json, payload, plen);
    json[plen] = '\0';

    LOG_I("set received: %s", json);

    root = cJSON_Parse(json);
    rt_free(json);
    if (!root)
    {
        LOG_E("set: JSON parse failed");
        return;
    }

    item = cJSON_GetObjectItem(root, "id");
    if (item && cJSON_IsString(item))
        rt_strncpy(msg_id, item->valuestring, sizeof(msg_id) - 1);

    params = cJSON_GetObjectItem(root, "params");
    if (params && cJSON_IsObject(params))
    {
        /* 灯 */
        v = unwrap_value(cJSON_GetObjectItem(params, "LED1"));
        if (v && cJSON_IsBool(v))
            ctl_led1(cJSON_IsTrue(v));

        /* 蜂鸣器 */
        v = unwrap_value(cJSON_GetObjectItem(params, "buzzer1"));
        if (v && cJSON_IsBool(v))
            ctl_buzzer(cJSON_IsTrue(v));

        /* 报警阈值 */
        v = unwrap_value(cJSON_GetObjectItem(params, "alarm_th"));
        if (v && cJSON_IsNumber(v))
            alarm_set_pm_threshold(v->valueint);
    }

    cJSON_Delete(root);

    /* 回执给平台 */
    rt_snprintf(reply, sizeof(reply),
        "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\"}", msg_id);
    mqtt_publish(c, DW_SET_REPLY, reply);

    /* 把状态同步回平台，方便小程序读取 */
    dw_post_control_state();
}

/* ── Upload: tem+hum -> dw, PM -> pm002 ── */
static void cloud_upload_data(struct env_sensor_data *data)
{
    char json[512];
    /* RT-Thread's rt_snprintf does NOT support %f, so format the temperature
     * manually as "%d.%d" (integer + one decimal). */
    float t = data->temp_valid ? data->temperature : 0.0f;
    int tenths = (int)(t * 10.0f + (t < 0 ? -0.5f : 0.5f));
    int t_ip = tenths / 10;
    int t_fp = tenths % 10;
    if (t_fp < 0) t_fp = -t_fp;

    if (g_dw_connected)
    {
        /* 每周期把 LED1/buzzer1/alarm_th 一并上报，小程序侧才能持续读到板子
         * 当前真实的控制状态（而不只是在下发后回一次）。 */
        rt_snprintf(json, sizeof(json),
            "{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{"
            "\"tem\":{\"value\":%d.%d},"
            "\"hum\":{\"value\":%d},"
            "\"LED1\":{\"value\":%s},"
            "\"buzzer1\":{\"value\":%s},"
            "\"alarm_th\":{\"value\":%d}}}",
            (unsigned long)(g_upload_count + 1),
            t_ip, t_fp,
            data->humi_valid ? (int)data->humidity : 0,
            g_led1_state   ? "true" : "false",
            g_buzzer_state ? "true" : "false",
            alarm_get_pm_threshold());

        if (mqtt_publish(&g_dw_client, DW_POST_TOPIC, json) != 0)
            g_upload_errors++;
    }

    if (g_pm002_connected)
    {
        rt_snprintf(json, sizeof(json),
            "{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{"
            "\"PM\":{\"value\":%d}}}",
            (unsigned long)(g_upload_count + 1),
            data->pm_valid ? (int)data->pms.pm2_5_cf : 0);

        if (mqtt_publish(&g_pm002_client, PM002_POST_TOPIC, json) != 0)
            g_upload_errors++;
    }

    g_upload_count++;
    LOG_I("Upload #%lu: tem=%d.%d hum=%d PM=%d",
          g_upload_count,
          t_ip, t_fp,
          data->humi_valid ? (int)data->humidity : 0,
          data->pm_valid ? (int)data->pms.pm2_5_cf : 0);
}

/* ── Main thread entry ── */
static void cloud_thread_entry(void *parameter)
{
    (void)parameter;
    struct env_sensor_data data;

    LOG_I("Cloud task started (RW007 WiFi + OneNET dual-device MQTT)");

    rt_wlan_register_event_handler(RT_WLAN_EVT_READY,
        wifi_ready_handler, RT_NULL);
    rt_wlan_register_event_handler(RT_WLAN_EVT_STA_DISCONNECTED,
        wifi_disconnect_handler, RT_NULL);

    /* Connect to the WiFi AP. rt_wlan_connect() blocks until associated (or
     * fails); RT_WLAN_EVT_READY fires later once DHCP assigns an IP. */
    if (!rt_wlan_is_connected())
    {
        LOG_I("Connecting to WiFi AP \"%s\" ...", WIFI_SSID);
        if (rt_wlan_connect(WIFI_SSID, WIFI_PASSWORD) == RT_EOK)
            LOG_I("WiFi associated, waiting for IP...");
        else
            LOG_W("WiFi connect failed, will retry");
    }

    while (g_cloud_running)
    {
        /* ── Block until WiFi is ready ── */
        if (!g_wifi_ready)
        {
            LOG_D("Waiting for WiFi connection...");
            rt_sem_take(g_net_sem, rt_tick_from_millisecond(CLOUD_UPLOAD_INTERVAL_MS));
            if (!rt_wlan_is_connected())
            {
                LOG_I("Reconnecting to WiFi AP \"%s\" ...", WIFI_SSID);
                rt_wlan_connect(WIFI_SSID, WIFI_PASSWORD);
            }
            continue;
        }

        /* ── Start the two MQTT clients once WiFi is up (they auto-reconnect) ── */
        if (!g_mqtt_started)
        {
            if (mqtt_client_init(&g_dw_client, DW_DEVID, DW_AUTH,
                                 dw_online_cb, dw_offline_cb,
                                 DW_SET_TOPIC, dw_property_set_cb) != RT_EOK)
                LOG_E("dw MQTT init failed");
            if (mqtt_client_init(&g_pm002_client, PM002_DEVID, PM002_AUTH,
                                 pm002_online_cb, pm002_offline_cb,
                                 RT_NULL, RT_NULL) != RT_EOK)
                LOG_E("pm002 MQTT init failed");
            g_mqtt_started = RT_TRUE;
            rt_thread_mdelay(3000);   /* give MQTT a moment to connect */
        }

        /* ── Wait until both devices are connected ── */
        if (!g_dw_connected || !g_pm002_connected)
        {
            LOG_D("Waiting for MQTT (dw=%d pm002=%d) ...",
                  g_dw_connected, g_pm002_connected);
            rt_thread_mdelay(1000);
            continue;
        }

        /* ── Read sensors & upload ── */
        if (sensor_get_latest(&data) == RT_EOK)
        {
            cloud_upload_data(&data);
        }
        else
        {
            g_upload_errors++;
            LOG_W("Failed to read sensor data");
        }

        rt_thread_mdelay(CLOUD_UPLOAD_INTERVAL_MS);
    }
}

/* ── Public API ── */
int cloud_task_init(void)
{
    if (g_cloud_running) return RT_EOK;

    g_net_sem = rt_sem_create("net_sem", 0, RT_IPC_FLAG_FIFO);
    if (!g_net_sem) return -RT_ERROR;

    g_cloud_running = RT_TRUE;
    g_upload_count = 0;
    g_upload_errors = 0;

    g_cloud_thread = rt_thread_create(
        "cloud", cloud_thread_entry, RT_NULL,
        CLOUD_THREAD_STACK_SIZE, CLOUD_THREAD_PRIORITY, 10);

    if (!g_cloud_thread)
    {
        rt_sem_delete(g_net_sem);
        g_net_sem = RT_NULL;
        g_cloud_running = RT_FALSE;
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
    if (g_net_sem)
    {
        rt_sem_delete(g_net_sem);
        g_net_sem = RT_NULL;
    }
    LOG_I("Cloud stopped. Sent: %lu, Errors: %lu", g_upload_count, g_upload_errors);
}

/* ── Finsh shell commands ── */
#ifdef RT_USING_FINSH
#include <finsh.h>

static void cmd_cloud_status(int argc, char *argv[])
{
    (void)argc; (void)argv;
    rt_kprintf("\n==== Cloud Upload Status ====\n");
    rt_kprintf(" Running:    %s\n", g_cloud_running ? "YES" : "no");
    rt_kprintf(" WiFi:       %s\n", g_wifi_ready   ? "UP" : "down");
    rt_kprintf(" dw MQTT:    %s  (tem + hum)\n", g_dw_connected    ? "Connected" : "Disconnected");
    rt_kprintf(" pm002 MQTT: %s  (PM)\n",       g_pm002_connected ? "Connected" : "Disconnected");
    rt_kprintf(" Uploaded:   %lu\n", g_upload_count);
    rt_kprintf(" Errors:     %lu\n", g_upload_errors);
    rt_kprintf("============================\n\n");
}
MSH_CMD_EXPORT_ALIAS(cmd_cloud_status, cloud_status, show cloud upload status);
#endif /* RT_USING_FINSH */
