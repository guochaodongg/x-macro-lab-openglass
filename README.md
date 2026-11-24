# XIAO ESP32S3 LED/BOOT/ST7789 LCD Demo（ESP-IDF v6.1）

精简版固件：只保留板载 LED 闪烁、BOOT 键深度睡眠开关机，以及 1.69 寸 ST7789 LCD 的 12 张万圣节图标轮播。已移除 Wi‑Fi 配网、网页服务、摄像头、强制门户等功能。

## 功能

- 上电后板载 LED 以 1 Hz 闪烁
- LCD 上电显示 12 张万圣节图标，每 2 秒切换一张
- 短按 BOOT 键进入深度睡眠，LED 和 LCD 背光关闭
- 再按 BOOT 键从深度睡眠唤醒，恢复 LED 闪烁和 LCD 显示

## 硬件

| 外设 | 说明 |
|---|---|
| 主控 | Seeed Studio XIAO ESP32S3（ESP32-S3R8，8 MB Flash，8 MB OPI PSRAM） |
| 用户 LED | GPIO21，橙色，低电平点亮 |
| BOOT 键 | GPIO0，低电平按下，RTC GPIO，用作 EXT0 唤醒源 |
| LCD | 金逸晨 1.69 寸 TFT，ST7789V2，240×280，SPI，10pin 带字库版本 |

### LCD 接线

10pin 屏幕请按丝印名称接线，不要按物理顺序猜。

| LCD 引脚 | XIAO 引脚 | ESP32-S3 GPIO |
|---|---|---|
| VCC | 3V3 | — |
| GND | GND | — |
| SCL / CLK | D8 | GPIO7 |
| SDA / DIN / MOSI | D10 | GPIO9 |
| CS | D6 | GPIO43 |
| DC / D/C | D7 | GPIO44 |
| RES / RST | D0 | GPIO1 |
| BLK / LED | D1 | GPIO2 |

10pin 带字库版本多出来的字库脚（常见标注 `F_CS`、`FONT_CS`、`CS1`、`NC` 或第二个 `GND`）当前不使用，可悬空；如果是第二个 `GND`，接到 GND。

LCD 参数在 `main/lcd_st7789.c` 顶部：

- SPI Mode 3
- 20 MHz
- Y 偏移 20
- 颜色反转
- BGR
- 12 张图标从源图 4×3 网格等大小裁剪，缩放为 240×240
- 图片以 16 色索引位图保存，循环轮播间隔 2 秒
- 图片在 240×280 屏幕中居中显示

## 目录结构

```text
.
├── main/
│   ├── CMakeLists.txt
│   ├── main.c              # LED 闪烁 + BOOT 深度睡眠
│   ├── lcd_st7789.h        # ST7789 LCD 驱动接口
│   ├── lcd_st7789.c        # ST7789 SPI 初始化 + 背光 + 12 张图标轮播
│   └── halloween_images.h  # 12 张 240×240 16 色索引图标数据
├── build.ps1               # 本机 ESP-IDF 构建脚本
├── flash.bat               # 使用 firmware.factory.bin 烧录
├── firmware.factory.bin    # 一体式固件
└── sdkconfig.defaults      # 默认配置
```

## 编译与烧录

本机 ESP-IDF 路径写在 `build.ps1` 中，当前为：

```powershell
$env:IDF_PATH = "D:\esp-idf\v6.1\esp-idf"
```

首次使用或切换芯片时：

```powershell
.\build.ps1 set-target
```

编译：

```powershell
.\build.ps1 build
```

烧录：

```bat
flash.bat COM3
```

或：

```powershell
.\build.ps1 fullflash -Port COM3
```

串口监视：

```powershell
.\build.ps1 monitor -Port COM3
```
