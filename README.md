# XIAO ESP32S3 LED/BOOT/ST7789 LCD Demo（ESP-IDF v6.1）

在 LED / BOOT 开关机 / ST7789 显示的基础上，新增通过 IP 网页上传照片并自动压缩适配屏幕的功能。

## 功能

- 上电后板载 LED 以 1 Hz 闪烁
- LCD 上电显示蜘蛛图片
- 短按 BOOT 键进入深度睡眠，LED 和 LCD 背光关闭
- 再按 BOOT 键从深度睡眠唤醒，恢复 LED 闪烁和 LCD 显示
- 手机/电脑浏览器打开设备 IP，选择任意尺寸照片，浏览器自动等比压缩到 240×280 后上传
- 设备解码 JPEG 并按屏幕比例居中显示，超出部分用黑边填充
- 可选把设备加入自有 Wi‑Fi（AP+STA），保存凭据后自动重连

## 网页上传照片

1. 手机或电脑连接 Wi‑Fi：`OpenGlass-Photo`，密码 `openglass123`
2. 浏览器打开 `http://192.168.4.1/`（连接设备到路由器后，也可用状态页显示的 STA IP 访问）
3. 点击或拖拽照片，页面会用 Canvas 按 240×280 等比缩放并导出 JPEG，再上传到设备
4. 上传完成后照片立即显示在屏幕上；`GET /photo` 可查看设备当前保存的照片

浏览器端压缩保证原图无论多大（几十 MP 也可以）都不会占用设备内存，设备只接收压缩后的小 JPEG。若从脚本直接上传原始 JPEG，设备端也会按 1/2、1/4、1/8 缩放解码后再适配屏幕，单次上传上限 2 MB。

页面底部的 Wi‑Fi 设置可保存路由器 SSID/密码；保存后设备以 AP+STA 模式工作，两个 IP 都能打开网页。AP 始终保留，避免路由器不可用时无法访问设备。

### HTTP 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 上传页面 |
| POST | `/upload` | `image/jpeg` 原始请求体，解码并显示 |
| GET | `/status` | JSON 状态（模式、IP、最近照片、空闲堆） |
| GET | `/photo` | 返回最近一次上传的 JPEG |
| POST | `/wifi` | `ssid=...&password=...` 保存并连接路由器 |

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
- 图片为白底黑蜘蛛单色位图，灰底水印已过滤
- 图片按原比例居中显示

## 目录结构

```text
.
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml   # esp_jpeg 依赖
│   ├── main.c              # LED、BOOT 深度睡眠与功能初始化
│   ├── lcd_st7789.h/.c     # ST7789 驱动 + 通用 RGB565 绘制
│   ├── spider_image.h      # 240×280 单色蜘蛛位图
│   ├── photo_wifi.h/.c     # AP+STA、凭据保存、强制门户 DNS
│   ├── photo_httpd.h/.c    # 上传页面与 HTTP 接口
│   ├── photo_display.h/.c  # JPEG 解码、缩放居中与显示
│   ├── dns_server.h/.c     # SoftAP DNS 重定向
│   └── web/upload.html     # 浏览器端压缩上传页面
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
