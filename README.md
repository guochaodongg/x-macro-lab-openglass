# XIAO ESP32S3 BOOT 键开关机 + 摄像头网页预览 Demo（ESP-IDF v6.1）

Seeed Studio XIAO ESP32S3 的 BOOT 键开关机 + 板载 LED 闪烁示例，使用本地 ESP-IDF v6.1 构建。开机后 LED 以 1Hz 闪烁；短按 BOOT 键进入深度睡眠（省电"关机"），再按一次 BOOT 键唤醒恢复运行。程序运行后会开启 Wi-Fi 热点，可通过浏览器访问设备 IP 查看运行状态、日志以及摄像头实时画面。

## 硬件说明

| 项目 | 值 |
|---|---|
| 主控 | ESP32-S3R8（双核 Xtensa LX7，240 MHz） |
| Flash | 8 MB |
| PSRAM | 8 MB OPI（已在配置中启用） |
| 用户 LED | **GPIO21，橙色，低电平点亮（active-low）** |
| BOOT 键 | **GPIO0，低电平按下（active-low），RTC GPIO，可作深度睡眠唤醒源** |
| 摄像头 | **OV2640（XIAO ESP32S3 Sense / OmiGlass 接口），JPEG 输出，VGA 640×480** |

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
│   └── main.c              # LED/BOOT 开关机 + Wi-Fi 网页状态与摄像头预览
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

## 网页查看运行状态与摄像头画面

程序启动后会创建一个 Wi-Fi 热点并启动本地网页服务：

| 项目 | 值 |
|---|---|
| Wi-Fi 名称（SSID） | `OpenGlass-Status` |
| Wi-Fi 密码 | `openglass123` |
| 浏览器地址 | `http://192.168.4.1/` |

使用方法：

1. 用电脑或手机连接热点 `OpenGlass-Status`
2. 打开浏览器访问 `http://192.168.4.1/`
3. 首页会直接显示摄像头实时画面，并在下方每秒刷新运行状态和最近日志

也可以直接访问：

- `http://192.168.4.1/stream`：MJPEG 实时视频流（可直接在浏览器打开）
- `http://192.168.4.1/snapshot`：抓取一帧 JPEG 图片
- `http://192.168.4.1/api/status`：纯文本状态信息
- `http://192.168.4.1/api/logs`：最近的运行日志

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

> Wi-Fi 名称、密码、端口和摄像头参数在 `main/main.c` 顶部宏中修改。按 BOOT 进入深度睡眠后，Wi-Fi、网页和摄像头都会停止，唤醒后自动恢复。

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
