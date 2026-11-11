# 🔥 星火一号环境监测系统 (SparkFire Env Monitor)

[![Platform](https://img.shields.io/badge/platform-STMicroelectronics-orange.svg)](https://www.st.com/)
[![MCU](https://img.shields.io/badge/MCU-STM32F407ZGT6-blue.svg)](https://www.st.com/en/microcontrollers-microprocessors/stm32f407zg.html)
[![RTOS](https://img.shields.io/badge/RTOS-RT--Thread%204.1.1-green.svg)](https://www.rt-thread.org/)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)

> 基于 [睿赛德星火一号 (STM32F407-RT-SPARK)](https://www.rt-thread.org/) 开发板的多传感器环境监测系统，实时采集温湿度与 PM2.5 数据，LCD 屏幕显示，支持蜂鸣器报警和云端数据上传。

<div align="center">
  <p>
    <b>🌡️ 温湿度采集</b> &nbsp;·&nbsp;
    <b>🌫️ PM2.5/PM10 监测</b> &nbsp;·&nbsp;
    <b>📺 LCD 实时显示</b> &nbsp;·&nbsp;
    <b>🚨 超限报警</b> &nbsp;·&nbsp;
    <b>📡 云端上传</b>
  </p>
</div>

---

## 2026年11月更新

作者杨斌，西华大学汽车服务工程专业。本人负责硬件连接、固件、云平台接入、小程序、系统调试与项目文档。

本次同步本地后续开发的 RW007 WiFi、OneNET 双设备 MQTT 上传、设备属性下发控制和微信小程序工程。小程序可查看实时数据与历史趋势，控制 LED、蜂鸣器，并调整 PM2.5 报警阈值。设备处理指令后回传控制状态。

![星火一号环境监测实物](docs/images/sparkfire-board.jpg)

固件位于 `applications/`，微信小程序位于 `miniprogram/`。设备连接配置使用本地配置文件，公开仓库提供示例模板。

2026年11月11日本机复核完成固件编译与链接，生成 `rtthread.bin`。小程序的签名、实时数据解析、控制请求、历史数据和网络失败处理完成离线模拟检查，5 个页面事件绑定通过核对。本次同步尚未重新进行开发板、小程序真机及真实云端联调，首次完整编译中第三方 LCD 和 OneNET 驱动有类型兼容性警告。

## 📋 功能特性

| 功能模块 | 说明 |
|----------|------|
| 🌡️ **温湿度采集** | AHT10/AHT20 传感器，通过 I2C3 (PE0/PE1) 采集，自动识别传感器型号 |
| 🌫️ **PM2.5 监测** | PMS5003 激光粉尘传感器，UART2 9600bps，自研帧解析器 |
| 📺 **LCD 显示** | 1.3 寸 240×240 ST7789V3 彩屏，8080 FSMC 并口，紧凑单页布局 |
| 🚨 **蜂鸣器报警** | 温度 > 30°C 或 PM2.5 > 70 μg/m³ 触发，响 10 秒后自动关闭 |
| 📡 **数据上传** | RW007 WiFi 连接 OneNET，通过两个 MQTT 客户端分别上报温湿度与 PM2.5，周期 5 秒 |
| 📱 **远程交互** | 微信小程序查看实时/历史数据，下发 LED、蜂鸣器和报警阈值设置，读取设备回传状态 |
| 🔧 **调试命令** | 10+ Finsh Shell 命令，支持传感器诊断、I2C 扫描、手动报警测试 |

## 🛠️ 硬件配置

### 开发板

- **型号**: 睿赛德星火一号 (STM32F407-RT-SPARK)
- **主控**: STM32F407ZGT6 (ARM Cortex-M4, 168MHz, 1MB Flash, 192KB SRAM)
- **OS**: RT-Thread v4.1.1

### 传感器与执行器

| 外设 | 型号 | 接口 | 引脚 | 参数 |
|------|------|------|------|------|
| 温湿度传感器 | AHT20 (板载) | I2C3 (软件) | SCL=PE0, SDA=PE1 | addr=0x38 |
| PM2.5 传感器 | PMS5003 (外接) | USART2 | TX=PA2, RX=PA3 | 9600-8N1 |
| LCD 显示屏 | ST7789V3 (板载) | FSMC 8080 | 16 位并口 | 240×240 |
| LED 矩阵 | SK6805 ×19 (板载) | TIM3_CH2 | PB5 | PWM DMA |
| 蜂鸣器 | 有源 (板载) | GPIO | PB0 | 高电平有效 |
| LED 供电 | GPIO | PF2 | LED_EN | 高电平使能 |
| LCD 背光 | GPIO | PF9 | BL_PWM | 高电平开启 |

### 引脚冲突处理 (⚠️ 重要)

本项目的关键技术挑战之一是多功能引脚的冲突解决。星火一号板载资源丰富，但 FSMC LCD 并口占用了大量引脚，与 I2C、SPI、LED 矩阵等存在多处冲突：

| 冲突引脚 | 功能 A | 功能 B | 解决方案 |
|----------|--------|--------|----------|
| PE0/PE1 | FSMC_NBL0/NBL1 | I2C3_SCL/SDA (AHT20) | 从 FSMC 配置中移除 (LCD 8080 不需要 NBL) |
| PF2 | FSMC_A2 | LED_EN | 从 FSMC 配置中移除，独立 GPIO 控制 |
| PB5 | SPI1_MOSI | TIM3_CH2 (SK6805) | 从 SPI1 配置中移除 |
| PE14 | FSMC_D11 | RW007 WiFi CS | 从 FSMC 配置中移除 |

详见 `board/CubeMX_Config/Src/stm32f4xx_hal_msp.c` 中的注释。

## 📐 软件架构

### 线程模型

```
优先级    线程名       栈大小   周期     职责
-------  -----------  ------  -------  ------------------------------
10       main         2048    -        主线程, 初始化各子系统
12       lcd_refresh  2048    2000ms   LCD 屏幕刷新
14       alarm        2048    500ms    报警条件检测 + 蜂鸣器控制
15       sensor       2048    2000ms   传感器数据采集 (AHT+PMS)
16       cloud        4096    5000ms   OneNET 双设备 MQTT 上传与下行控制
```

### 数据流

```
AHT20 ──I2C3──┐
               ├──> sensor_task ──mutex──> 共享数据 ──┬──> lcd_refresh ──> LCD
PMS5003─UART2─┘                                      ├──> alarm_task  ──> Buzzer
                                                     └──> cloud_task  ──> RW007 WiFi ──> OneNET ──> 微信小程序
```

### 源码组织

```
applications/
├── main.c              # 主函数, LCD 启动自检, Finsh 调试命令
├── sensor_task.h/c     # 传感器数据结构 + AHT20 I2C + PMS5003 帧解析器
├── alarm_task.h/c      # 蜂鸣器报警控制 (双阈值 10s 限时)
├── lcd_display.h/c     # LCD 单页显示 (温湿度/PM/粒子数/AQI)
├── cloud_task.h/c      # 双设备 MQTT 上传、下行控制与状态回传
└── cloud_config.example.h # WiFi 与设备鉴权配置模板
```

### PMS5003 帧解析器

项目使用自研的 32 字节帧解析器，不依赖第三方软件包：
- 滑动窗口缓冲接收
- 起始字节验证 (0x42 0x4D)
- 帧长度验证 (28 字节 data)
- 大端校验和验证
- 解析 12 个数据字段 (PM1.0/2.5/10 × CF/std, 粒子数 ×6)

### AHT10/AHT20 自动识别

AHT10 和 AHT20 使用不同的初始化命令集，系统自动通过多轮探测识别：

| 命令 | AHT10 | AHT20 |
|------|-------|-------|
| Soft Reset | 0xE1 | 0xBA |
| Calibrate | 0xBE 0x08 0x00 | 0xE1 0x08 0x00 |
| Trigger | 0xAC 0x33 0x00 | 0xAC 0x33 0x00 |

## 🔨 快速开始

### 环境要求

- **RT-Thread Studio** (推荐) 或 RT-Thread Env + MDK5
- STM32F4 HAL 库 (已包含在 `libraries/` 中)
- ST-LINK 调试器 (板载)

### 配置

1. 将 `applications/cloud_config.example.h` 复制为 `applications/cloud_config.h`，填写自己的 WiFi、OneNET 产品和设备鉴权信息。
2. 当前固件通过 `dw` 设备上报 `tem`、`hum`、`LED1`、`buzzer1`、`alarm_th`，通过 `pm002` 设备上报 `PM`，在 OneNET 中创建对应物模型。
3. 在 RT-Thread Studio 或 Env 中恢复 `.config` 对应的软件包，包清单见 `packages/pkgs.json`。本地构建使用已有的 `packages/` 依赖缓存。RW007 配置已按实际源码核对为 v2.0.1。
4. 在自己的编译环境中设置 `RTT_EXEC_PATH` 指向 GNU Arm Embedded 工具链的 `bin` 目录。
5. 小程序导入与本地鉴权配置见 [小程序说明](miniprogram/README.md)。

### 编译

```bash
# 完成配置和软件包准备后，在项目根目录执行
scons -j4
```

编译产物：
- `rtthread.bin` — 固件文件 (烧录到 0x08000000)
- `rt-thread.elf` — ELF 文件 (调试用)
- `rt-thread.map` — 内存映射文件

### 烧录

使用 STM32CubeProgrammer CLI：

```bash
STM32_Programmer_CLI -c port=SWD -w rtthread.bin 0x08000000 -v -rst
```

或使用 RT-Thread Studio 内置的下载功能。

### 运行

1. 使用 USB 线连接开发板的 ST-LINK USB 口
2. 打开串口终端 (115200-8N1)，连接 ST-LINK 虚拟串口
3. 按下 RESET 键，观察启动日志
4. 在 Finsh Shell 中输入命令进行交互

## 💻 Finsh Shell 命令

| 命令 | 功能 |
|------|------|
| `cmd_sensor` | 查看当前温湿度 + PM2.5 + 粒子数详细数据 |
| `aht10_test [1]` | AHT20 传感器诊断 (参数 1=强制重新初始化) |
| `i2c_scan [bus]` | I2C 总线设备扫描 (默认 i2c3) |
| `cmd_uart2_test` | UART2 硬件层轮询测试 (PMS5003 帧检测) |
| `buzzer <on\|off>` | 蜂鸣器手动控制 |
| `alarm_status` | 报警系统状态查看 |
| `cloud_status` | 云上传状态查看 |
| `cmd_lcd_reset` | 重置 LCD 显示 |
| `list_device` | RT-Thread 设备列表 |

## 📄 许可证

本项目基于 [Apache License 2.0](LICENSE)。

包含的第三方代码：
- RT-Thread 内核 (`rt-thread/`) — Apache 2.0
- STM32F4 HAL 驱动 (`libraries/`) — BSD 3-Clause
- 板级驱动 (`libraries/Board_Drivers/`) — Apache 2.0

## 🙏 致谢

- [RT-Thread](https://www.rt-thread.org/) — 优秀的国产开源物联网操作系统
- [睿赛德科技](https://www.rt-thread.com/) — 星火一号开发板
- [STMicroelectronics](https://www.st.com/) — STM32F4 平台与 HAL 库

---

*项目创建 2026-06-29，后续工程同步 2026-11-11。作者 [young03060](https://github.com/young03060)。*
