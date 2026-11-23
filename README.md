# XIAO ESP32S3 BOOT 键开关机 + 摄像头网页预览 + ST7789 LCD Demo（ESP-IDF v6.1）

Seeed Studio XIAO ESP32S3 的 BOOT 键开关机 + 板载 LED 闪烁示例，使用本地 ESP-IDF v6.1 构建。开机后 LED 以 1Hz 闪烁；短按 BOOT 键进入深度睡眠（省电"关机"），再按一次 BOOT 键唤醒恢复运行。首次使用默认连接 `pura80` / `88888888`，也支持通过配网热点在浏览器中重新配置 Wi‑Fi。断电重启后自动连接并继续提供运行状态、日志、摄像头实时画面和 1.69 寸 ST7789 LCD 测试显示。

## 硬件说明

| 项目 | 值 |
|---|---|
| 主控 | ESP32-S3R8（双核 Xtensa LX7，240 MHz） |
| Flash | 8 MB |
| PSRAM | 8 MB OPI（已在配置中启用） |
| 用户 LED | **GPIO21，橙色，低电平点亮（active-low）** |
| BOOT 键 | **GPIO0，低电平按下（active-low），RTC GPIO，可作深度睡眠唤醒源** |
| 摄像头 | **OV2640（XIAO ESP32S3 Sense / OmiGlass 接口），JPEG 输出，VGA 640×480** |
| LCD | **金逸晨 1.69 寸 TFT，ST7789V2，240×280，SPI，3.3V，10pin 带字库版本** |

### 1.69 寸 ST7789 LCD 接线

| LCD 引脚 | XIAO 引脚 | ESP32-S3 GPIO | 说明 |
|---|---|---|---|
| VCC | 3V3 | — | **必须 3.3V，不要接 5V** |
| GND | GND | — | 地 |
| SCL / CLK | D8 | GPIO7 | SPI 时钟 |
| SDA / DIN / MOSI | D10 | GPIO9 | SPI 数据 |
| CS | D6 | GPIO43 | 片选 |
| DC / D/C | D7 | GPIO44 | 数据/命令选择 |
| RES / RST | D0 | GPIO1 | 复位 |
| BLK / LED | D1 | GPIO2 | 背光控制，高电平点亮 |

> 10pin 带字库版本请按**丝印名称**接线，不要按物理顺序猜。多出来的字库引脚（常见标注 `F_CS` / `FONT_CS` / `CS1` / `NC` / 第二个 `GND`）只用于读取模块上的字库芯片，**不影响 ST7789 点亮和彩条显示**，当前驱动可以悬空不接。
> 以上显示引脚避开了摄像头和板载 LED。若你的屏幕已硬连线到其他 GPIO，修改 `main/lcd_st7789.c` 顶部的 `LCD_PIN_*` 宏后重新编译。
> 当前默认参数：SPI Mode 3、20 MHz、Y 偏移 20、颜色反转、BGR 顺序；上电后会显示提供的蜘蛛图片（已转换为 240×280 单色位图，灰底水印已过滤）。

> `gpio_set_level(21, 0)` → LED 亮；`gpio_set_level(21, 1)` → LED 灭。
> 这是 XIAO ESP32S3 的硬件设计，与常见开发板的高电平点亮相反。

## 目录结构

```
.
├── CMakeLists.txt          # ESP-IDF 顶层工程文件
├── sdkconfig.defaults      # 目标配置：8MB Flash / OPI PSRAM / USB Serial/JTAG 控制台
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml   # esp32-camera 组件依赖声明
│   ├── main.c              # LED/BOOT 开关机 + Wi-Fi 配网/网页状态/摄像头预览
│   ├── dns_server.h        # 强制门户 DNS 服务声明
│   ├── dns_server.c        # 强制门户 DNS 服务（所有域名解析到 192.168.4.1）
│   ├── lcd_st7789.h        # ST7789 LCD 驱动接口
│   ├── lcd_st7789.c        # 1.69 寸 ST7789 SPI 驱动 + 背光 + 蜘蛛图片
│   └── spider_image.h      # 240×280 单色蜘蛛位图数组
├── dependencies.lock       # 组件管理器生成的依赖锁文件
├── managed_components/     # 组件管理器下载的 esp32-camera 等组件
├── build.ps1               # 本机 ESP-IDF 环境辅助脚本（编译/烧录/监视）
├── flash.bat               # 用预编译固件一键烧录
├── firmware.factory.bin    # 一体式固件（bootloader + 分区表 + 应用）
└── build/                  # 编译产物（.gitignore 已忽略）
```

## 快速开始

### 方式 A：用预编译固件直接烧录（最简单）

1. 用 USB-C 数据线连接 XIAO ESP32S3 到电脑
2. 查看设备管理器 → 端口，找到 `USB JTAG/serial debug unit (COMx)`
3. 运行：

```bat
flash.bat COM3
```

把 `COM3` 换成你的实际端口。烧录完成后板子自动重启，橙色 LED 开始以 1Hz 闪烁。

### 方式 B：改代码后重新编译烧录

在项目目录打开 **PowerShell**：

```powershell
.\build.ps1 build                    # 编译
.\build.ps1 flash -Port COM3         # 烧录
.\build.ps1 monitor -Port COM3       # 打开串口监视器（查看 printf 输出）
.\build.ps1 fullflash -Port COM3     # 烧录 + 监视一步到位
```

首次使用或切换芯片时先执行 `.\build.ps1 set-target`。

> 首次编译会通过 ESP-IDF 组件管理器下载 `espressif/esp32-camera` 和 `espressif/esp_jpeg`，需要联网；下载结果保存在 `managed_components/`，依赖版本记录在 `dependencies.lock`。

> 如果 PowerShell 提示脚本被禁止运行，先执行：
> `Set-ExecutionPolicy -Scope Process -ExecutionPolicy RemoteSigned`

### 方式 C：用官方 ESP-IDF 终端

如果你有自己的 ESP-IDF 环境（"ESP-IDF PowerShell" 快捷方式）：

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

## 网页配网、运行状态与摄像头画面

设备采用“先配网、后自动连接”的方式，配网流程参考小智固件（`78/esp-wifi-connect`）的方案，加入**强制门户（Captive Portal）**、**先验证后保存**和**独立配网页面**：

1. 首次使用且 NVS 中没有保存热点时，设备会自动写入默认热点 `pura80` / `88888888` 并尝试连接
2. 配网热点 `OpenGlass-Status` 始终开启作为保底配置入口；若默认热点不可用或需要连接其他热点，可连接该热点
3. 连接配网热点后，**大多数设备会自动弹出配网页面**；如果没有自动弹出，浏览器手动打开 `http://192.168.4.1/provision`
   - 原理：设备内置 DNS 服务会把所有域名解析到 `192.168.4.1`，并把系统的联网探测请求（iOS/Android/Windows）302 跳转到配网页面
4. 配网页面：扫描热点、输入密码、连接并保存；此时不显示摄像头和状态日志
5. 设备会先尝试连接目标热点，**验证成功（拿到 IP）后才会把凭据保存到 NVS**；验证失败会提示原因且不覆盖已保存的凭据
   - 为应对 APSTA 模式下信道切换导致的首次关联失败，会自动重试 2 次（间隔 3 秒）
6. 配网成功后，页面自动跳转到功能页 `/app`
7. 已配置设备再次上电时，`/` 直接进入功能页；需要重新配网可访问 `/provision` 或点击功能页上的“重新配网”
8. 断电再上电后设备会自动连接已保存热点，并在功能页状态区显示路由器/手机热点分配的局域网 IP；断线后会自动重连

配网热点与页面：

| 项目 | 值 |
|---|---|
| Wi-Fi 名称（SSID） | `OpenGlass-Status` |
| Wi-Fi 密码 | `openglass123` |
| 配网页面 | `http://192.168.4.1/`（未配置时）或 `http://<设备IP>/provision` |
| 功能页面 | `http://<设备IP>/app` |
| 强制门户 | DNS 劫持 + 联网探测 302 跳转（自动弹页） |

连接成功后，配网热点仍然保留，方便随时重新配置；功能页同时可通过两个地址访问：

- `http://192.168.4.1/app`：配网热点地址（始终可用）
- `http://<状态页显示的局域网 IP>/app`：已连接热点分配的 IP

功能页内容：

- 摄像头实时预览（81 端口 MJPEG 流）和单帧抓拍入口
- 每秒刷新的运行状态（含“配网状态/配网信息”）和最近日志
- “重新配网”链接，跳转到 `/provision`

也可以直接访问：

- `http://192.168.4.1/api/wifi/scan`：扫描附近热点（JSON）
- `http://<设备IP>:81/stream`：MJPEG 实时视频流（独立服务，不阻塞配网页面）
- `http://<设备IP>/stream`：80 端口兼容地址，返回单帧 JPEG
- `http://<设备IP>/snapshot`：抓取一帧 JPEG 图片
- `http://<设备IP>/api/status`：纯文本状态信息，含 STA IP
- `http://<设备IP>/api/logs`：最近的运行日志

> Wi-Fi 凭据以明文形式保存在 ESP32 的 NVS 中，仅适合本地/IoT 演示用途。视频流使用独立 81 端口，`sdkconfig.defaults` 已将 `CONFIG_LWIP_MAX_SOCKETS` 提高到 20，避免 socket 耗尽导致 `accept (23)`。

摄像头默认配置：

| 项目 | 值 |
|---|---|
| 传感器 | OV2640（自动识别兼容型号） |
| 输出格式 | JPEG |
| 分辨率 | VGA 640×480 |
| 帧缓冲 | 2 帧，放在 PSRAM |
| XCLK | 20 MHz |
| SCCB | SDA=GPIO40，SCL=GPIO39 |
| DVP | D0~D7=GPIO15/17/18/16/14/12/11/48，VSYNC=GPIO38，HREF=GPIO47，PCLK=GPIO13，XCLK=GPIO10 |

> 默认连接的热点在 `main/main.c` 的 `DEFAULT_WIFI_SSID` / `DEFAULT_WIFI_PASSWORD` 宏中修改，仅在 NVS 未保存热点时写入一次；之后可通过 `/provision` 网页覆盖。配网热点名称/密码、HTTP 端口和摄像头参数也在文件顶部宏中。按 BOOT 进入深度睡眠后，Wi-Fi、网页和摄像头都会停止，唤醒后自动恢复。

## BOOT 键开关机说明

| 操作 | 效果 |
|---|---|
| 插电 / 复位 | 正常运行，LED 以 1Hz 闪烁 |
| 短按 BOOT | 进入深度睡眠（LED 熄灭，功耗降到微安级，相当于"关机"） |
| 再短按 BOOT | 从深度睡眠唤醒，恢复正常运行 |

实现要点：

- BOOT 键接在 GPIO0（RTC GPIO），内部上拉、低电平按下；深度睡眠时配置为 `EXT0` 唤醒源，按下即唤醒。
- 按键检测做了 50ms 消抖；唤醒后会先等待按键松开，避免"刚唤醒又被按睡"。
- 开机或唤醒时串口会打印当前状态，可用 `.\build.ps1 monitor -Port COM3` 查看。
- 如果想防止误触关机，可把 `main/main.c` 里的 `POWER_KEY_HOLD_MS` 从 `0` 改成 `1000`，这样需要长按 1 秒才关机（开机唤醒仍是短按）。

> 说明：深度睡眠通过切断大部分芯片电源来省电，但 XIAO 板载的 USB 转串口等外围器件仍会消耗少量电流。若要做到真正的"物理断电"，需要在硬件上用 MOSFET 做自锁电源电路。

## 环境说明

`build.ps1` 已针对这台机器的 ESP-IDF 布局做了适配：

- IDF 源码：`D:\esp-idf\v6.1\esp-idf`
- 工具链：`D:\esp-idf\v6.1\tools`（xtensa-esp-elf 15.2.0）
- Python 虚拟环境：`C:\Users\guochaodong\.espressif\python_env\idf6.1_py3.11_env`

由于本机工具（D 盘）与 Python 虚拟环境（C 盘）不在同一目录，官方 `export.bat`/`activate.py` 无法自动激活，因此脚本手动注入了所需的环境变量。换机器时需修改脚本开头的路径。

## 常见问题

**找不到串口 / 烧录失败**
按住板子上的 **BOOT** 键不放，插入 USB 线，再松开 BOOT，使其进入下载模式。XIAO ESP32S3 的原生 USB 口在设备管理器中通常显示为 `USB JTAG/serial debug unit`。

**LED 不亮或常亮**
确认 GPIO21 的逻辑是反相的（低电平亮）。本示例已按 active-low 编写。

**串口没有输出**
`sdkconfig.defaults` 已启用 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`，`printf` 会输出到 USB-C 口。若使用外部 USB-UART 转换器接在 D6/D7（GPIO43/44），需要在 menuconfig 中把控制台改回 UART。

**摄像头画面不可用**
查看串口日志中 `[camera]` 开头的行。若提示初始化失败，请确认摄像头是 OV2640 兼容模块且接线与 `main/main.c` 中的 `CAMERA_PIN_*` 一致；若画面上下颠倒或左右镜像，修改 `CAMERA_VFLIP`、`CAMERA_HMIRROR` 后重新编译烧录。

## 已移除的环境

原先安装的 PlatformIO 6.2.0（pip 包与全局缓存 `C:\Users\guochaodong\.platformio`，约 2.1GB）已全部清除，本项目不再依赖 PlatformIO。
