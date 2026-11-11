# 星火一号环境监测小程序

原生微信小程序，通过 OneNET 查询星火一号设备状态与历史数据，下发设备属性设置。

## 已有功能

- 温度、湿度和 PM2.5 实时显示
- 设备在线状态、数据更新时间与请求失败反馈
- 1 小时、6 小时和 24 小时 PM2.5 历史趋势
- LED、蜂鸣器控制和 PM2.5 报警阈值设置
- 控制后读取设备回传的状态

## 配置与预览

1. 将 `utils/onenet.config.example.js` 复制为 `utils/onenet.config.js`，填写自己的产品 ID、用户 ID、访问密钥和设备名。
2. 在微信开发者工具中导入本目录，按自己的微信项目设置 AppID。公开配置使用 `touristappid`。
3. 在 OneNET 物模型中配置下表中的属性，使其与固件一致。
4. 查询和控制使用 `https://iot-api.heclouds.com`。真机运行时按微信平台要求配置请求域名。

| 设备 | 属性标识 | 类型与用途 |
| --- | --- | --- |
| `dw` | `tem`、`hum` | 温度、湿度 |
| `dw` | `LED1`、`buzzer1` | 布尔值，LED 与蜂鸣器控制状态 |
| `dw` | `alarm_th` | 整数，PM2.5 报警阈值 |
| `pm002` | `PM` | PM2.5 浓度 |

设备名可在本地配置中修改，同时需要与固件 `cloud_config.h` 保持一致。

当前鉴权沿用项目的 OneNET 直连方案。本地配置含访问密钥，已经加入 Git 忽略规则。

## Python 查询工具

`test_onenet.py` 沿用同一套签名与物模型，用于手动查询设备和历史数据。运行前在本地环境设置 `ONENET_PRODUCT_ID`、`ONENET_USER_ID` 和 `ONENET_ACCESS_KEY`。

```powershell
python test_onenet.py
```

默认入口执行查询。控制操作保留在 `set_device` 函数中，手动调用时会改变真实设备状态。
