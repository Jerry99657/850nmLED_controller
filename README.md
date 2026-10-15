# 850 nm 红外 LED 控制验证板

本项目用于验证 50 颗三芯 5050 850 nm 灯珠（共 150 颗红外 LED 芯片）的恒流驱动、统一调光、5 V/9 V 供电兼容、温度保护与显示交互。

主控为 **ESP32-C3-WROOM-02-N4**。所有红外 LED 同步调光，不支持单灯独立控制。

## 硬件概要

- 红外阵列：15 路恒流串，每路 10 颗 850 nm LED 芯片；共 150 颗。
- LED 驱动：3 × `LP8556SQ-E09`，每颗使用 5 路恒流下拉通道；每路目标电流约 39.4 mA。
- 调光方式：ESP32-C3 输出一组公共 PWM，同时控制三颗 LP8556。
- 功率输入：`HUSB237` USB-C 取电，VSET = 75 kΩ，优先请求 9 V。
- 控制电源：`AP63203` 降压输出 3.3 V。
- 显示与指示：0.96 英寸 SPI TFT、WS2812-V6 状态灯、0805 红色功率随动指示灯。
- 检测：`TMP102` 温度传感器、VBUS 电阻分压采样。

> 5 V 输入可用，但固件必须限制最大 PWM 占空比；9 V 输入才允许满功率运行。

## ESP32-C3 IO 分配

| GPIO | 网络/功能 | 软件外设与说明 |
|---:|---|---|
| GPIO0 | `ADC0`，VBUS 电压采样 | ADC1_CH0；分压比 0.332，5 V 约 1.66 V，9 V 约 3.0 V（超出校准量程，只用于判档） |
| GPIO1 | `LED_PWM` | LEDC PWM；三颗 LP8556 的公共调光输入，同时驱动 0805 红色指示灯 |
| GPIO2 | `KEY_UP` | 低有效按键；启动绑带脚，外部上拉 |
| GPIO3 | `TFT_DC` | SPI 数据/命令选择 |
| GPIO4 | `I2C_SDA` | TMP102；预留 LP8556 I²C 总线 |
| GPIO5 | `I2C_SCL` | I²C 时钟，建议最高 400 kHz |
| GPIO6 | `TFT_SCLK` | SPI 时钟 |
| GPIO7 | `TFT_MOSI` | SPI 数据输出 |
| GPIO8 | `WS2812` | RMT 外设或 SPI 模拟时序；启动绑带脚，外部上拉 |
| GPIO9 | `KEY_DOWN/BOOT` | 低有效按键；复位时按住会进入下载模式 |
| GPIO10 | `TFT_CS` | SPI 片选 |
| GPIO18 | `USB_D-` | 原生 USB Serial/JTAG 下载与日志 |
| GPIO19 | `USB_D+` | 原生 USB Serial/JTAG 下载与日志 |
| GPIO20 | `UART_RXD` | UART 调试/测试点 |
| GPIO21 | `UART_TXD` | UART 调试/测试点 |

`GPIO2`、`GPIO8`、`GPIO9` 为启动绑带脚。按键应接地并依赖外部上拉；GPIO9 不应外接较大电容。

## 外设说明

| 外设 | 接口 | 使用方式 |
|---|---|---|
| LP8556 × 3 | 公共 PWM，I²C 预留 | E09 按 PWM-only 使用；三颗器件 I²C 地址均为 `0x2C`，不可分别读取状态 |
| TMP102 | I²C，地址 `0x48` | 定期读取板上局部温度，用于显示与热保护 |
| 0.96 英寸 TFT | 4 线 SPI | 显示输入电压、亮度、温度、供电模式和故障信息 |
| WS2812-V6 | 单线数据 | 显示初始化、5 V 降额、9 V 正常、预警与故障状态 |
| 0805 红色 LED | GPIO1 公共 PWM | 经 1 kΩ 限流，亮度跟随 PWM 指令，占空比为 0 时熄灭 |
| USB Serial/JTAG | GPIO18/19 | 固件下载、日志输出与调试 |
| UART | GPIO20/21 | 预留外部串口调试与测试点 |

## 计划实现的固件功能

1. 上电后保持 `LED_PWM = 0`，初始化外设并显示系统状态。
2. 采样 VBUS，自动识别 5 V 与 9 V 输入。
3. 两档都允许最大 100% PWM（原计划 5 V 限 50%，现已放开，上限值在 menuconfig “VBUS detection” 里可调）。
4. 使用 LEDC 产生 1～5 kHz 公共 PWM，并使用 300～500 ms 软启动。
5. 读取 TMP102：55 ℃以上预警/降额，65～70 ℃强制降额，70 ℃关断；最终阈值以热测试结果为准。
6. TFT 显示电压、温度、亮度设定值、供电模式及故障信息。
7. 两个按键用于亮度调节；短按步进，长按连续调节。
8. WS2812 显示运行状态：蓝色初始化、绿色 9 V 正常、黄色 5 V 降额、橙色预警、红色故障。
9. 在 VBUS 异常、温度超限或主控复位时立即关闭 PWM。

## 建议开发环境

使用 **VS Code + Espressif IDF 扩展 + ESP-IDF**，Target 选择 `esp32c3`。

推荐先分阶段验证：

1. USB 下载与串口日志。
2. GPIO1 PWM 与 0805 红灯。
3. I²C TMP102。
4. SPI TFT 与按键、WS2812。
5. VBUS ADC 采样与 5 V/9 V 模式切换。
6. 单颗 LP8556 低占空比测试，再逐步接入三组驱动和全部灯串。

## 固件工程

ESP-IDF v6.1，target `esp32c3`。日志同时输出到 UART0（GPIO20/21）和 USB Serial/JTAG。

```text
CMakeLists.txt            工程入口，PROJECT_VER 为固件版本
sdkconfig.defaults        target / 4 MB Flash / 控制台 / 1 kHz tick / 1500K 应用分区
components/
  kk_oled/                KK_OLED 图形核心（MIT）+ ST7735S 驱动 driver/kk_oled_driver.c
  kk_ui/                  KK_UI 菜单 UI（MIT），布局常量已按 160x80 调整
main/
  app_main.c              初始化与 5 ms 主循环、I²C 扫描、屏幕测试图、心跳日志
  control.c/.h            亮度设定、非阻塞软启动、TMP102 过温保护、WS2812 状态灯、定时关断
  vbus.c/.h               VBUS ADC 采样（5 V / 9 V 判档）
  lp8556.c/.h             LP8556 LED_EN 配置（只写不读）
  settings.c/.h           设置掉电保存（NVS）
  remote.c/.h             Wi-Fi 热点 + HTTP 接口 + 强制门户 + 热点/蓝牙互斥管理，命令经队列交主循环执行
  ble_link.c/.h           蓝牙 BLE 外设（NimBLE），GATT 命令/状态特征
  web/index.html          手机控制网页（嵌入固件）
  led_pwm.c/.h            GPIO1 LEDC 公共调光（千分比、上限钳位、立即关断）
  tmp102.c/.h             TMP102 温度读取（i2c_master 驱动）
  ws2812.c/.h             WS2812 状态灯（RMT）
  led_strip_encoder.c/.h  RMT 编码器，取自 IDF 示例 rmt/led_strip
  （components/dns_server 取自 IDF 示例 captive_portal，Unlicense/CC0）
  ui_input.c/.h           两键 → KK_UI 上/下/确定（两键同按 = 确定）
  ui_app.c/.h             KK_UI 页面：运行 / 设置 / 状态 / 关于
  ui_fonts.c/.h/.json     LEDFont 生成的文泉驿字模子集
  ui_icons.h              首页 32x32 图标（取自 KK_UI 标准 Demo）
  board_pins.h            GPIO 分配
  Kconfig.projbuild       menuconfig → "850nm LED Controller"
tools/gen_ui_fonts.py     重新生成 ui_fonts.c
tools/ble_control.html    手机蓝牙控制网页（单文件，Android Chrome）
```

当前完成阶段 1~5，并加入手机遥控：

- 操作：KEY_UP 在左、KEY_DOWN 在右，按物理位置使用。左键 = 上一项 / 减小 / 关 / 取消，右键 = 下一项 / 增大 / 开 / 确定，长按连发；两键同时按 = 确定 / 返回。
- 首页：运行、设置、状态、关于。
- 运行页：标题显示 5V/9V 档位，大号显示实际输出，进度条竖线标出当前上限；UP/DOWN 按“调光步进”调设定值。
- VBUS 判档（阶段 5）：每 100 ms 采样，VBUS > 7.5 V 连续 3 次判为 9 V，< 6.5 V 立即判为 5 V；只插下载口、ADC 故障时按 5 V。降档时输出立即压到该档上限，档位变化弹出提示。阈值、回差、分压比、两档上限在 menuconfig “VBUS detection”，目前 5 V / 9 V 上限都是 100%（即不限幅）。
- 上电 `LED_PWM = 0%`，只有在运行页按右键（KEY_DOWN）才会开启；0 → 上限按软启动时间（默认 400 ms）非阻塞爬升。
- TMP102 每 500 ms 读取一次；≥70 ℃ 或连续 3 次读不到时立即关断 PWM 并锁存故障，回落到 55 ℃ 以下解除，亮度设定清零，需重新按键开启。故障状态变化在任意页面弹出提示。
- 设置页：温度保护开关、软启动时间、调光步进、状态灯开关与亮度、恢复默认；全部掉电保存到 NVS（`main/settings.c`）。
- 温度保护关闭后不再因过温或传感器故障关断，但仍受占空比上限约束；运行页标题显示“保护关闭”。该状态也会保存，上电时为关闭会弹出“温度保护已关闭”提示；“恢复默认”会重新开启。
- WS2812：故障黄色闪烁 > PWM 输出红色（亮度 = 占空比 × 50%）> 温度预警橙色 > 页面颜色（首页蓝、运行绿、设置紫、状态青、关于白）。故障和 PWM 指示不受状态灯开关影响。
- 离开运行页（两键同按返回）时 LED_PWM 立即归零，不走软启动斜坡；其它页面不允许输出。
- 启动时 I²C 先检查 SDA/SCL 空闲电平，再扫描全部地址，TMP102 在 0x48~0x4B 中自动识别。

- LP8556：上电默认 LED_EN（16h）= 0x3F 六路全开，本板只接 5 路。每次检测到 VBUS 上电后，在 PWM 为低（Standby）时写 LED_EN = 0x1F 关闭悬空的 LED6，写入成功前不允许输出；输出中接入功率口会先停止输出再配置，需重新开启。VBUS 有电但持续 2 s 写不进去则锁存“驱动故障”。只插下载口时 LP8556 无供电，不影响 LED52 测试。状态页“LED驱动”显示配置状态。
- 手机遥控：板子开启 WPA2 热点（SSID `850nm-xxxx`，默认密码 `850nm1234`，都显示在“关于”页），手机连接后会自动弹出控制页（强制门户：DNS 全部解析到本机、未知地址重定向、DHCP Option 114）；没有弹出时手动在浏览器打开 `http://192.168.4.1`。系统提示“此网络无法上网”时选择保持连接：
  - 设定占空比（0~100%，超过当前上限会被钳位）；设定 > 0 时板子自动进入运行页。
  - 定时关断（1~999 分钟，输出开启后才能设定）；运行页标题栏显示倒计时，到时自动归零并提示。
  - 停止输出；页面每秒刷新输出、上限、VBUS、温度、保护和剩余时间。
  - 停止、故障、离开运行页或定时到时都会同时取消定时。板上按键暂不能设定定时。
  - 上电顺序：先完成屏幕、传感器等小电流外设，Wi-Fi 在后台延时 3 s 启动（上次欠压复位则 9 s），发射功率限制为 10 dBm，欠压复位后自动再降功率，降低 3.3 V DCDC 的瞬态压降。蓝牙未编译进固件。
  - 安全：只有 Wi-Fi 密码一层保护，HTTP 无认证，请在 menuconfig “Wi-Fi remote (phone)” 中修改密码；也可在该菜单关闭遥控。

- 蓝牙控制：与热点同名广播（`850nm-xxxx`）。Android 手机用 Chrome 打开 `tools/ble_control.html`，点“连接板子”选择设备，功能与热点网页相同，手机可同时正常上网。
  - 蓝牙连上后自动关闭热点，断开 3 s 后恢复；有手机连在热点上时停止蓝牙广播。两者都空闲时同时开启，任一方式都能连上。
  - 网页蓝牙需要安全上下文：Android 上从文件管理器打开、地址为 `content://` 时不可用，需放到 HTTPS 网址（如 Gitee Pages）；电脑 Chrome 可直接打开本地文件。iPhone Safari 不支持。
  - 蓝牙未加密，附近任何人都能连接并控制红外输出。可在 menuconfig “Wi-Fi remote (phone)” 关闭。

menuconfig：PWM 频率/软启动/上限、过温阈值、状态灯默认亮度在 "850nm LED Controller"；屏幕参数在 "KK_OLED: ST7735S"。`LED_PWM_DEMO`（阶梯测试）默认关闭，开启后会自动点亮红外阵列。

屏幕调整：驱动工作在面板原生竖屏方向，横屏由软件旋转。启动时先显示 1.5 s 测试图（边框 + 对角线 + 左上角方块 + “160x80”）：

- 边框缺一边或有杂色条 → 调整 `TFT_COL_OFFSET` / `TFT_ROW_OFFSET`（132x162 RAM 面板常见 26/1，128x160 RAM 面板常见 24/0）；
- 画面上下颠倒 → 切换 `TFT_ROTATE_180`（默认已打开，适配当前安装方向）；
- 底色发白、颜色反相 → 关闭 `TFT_INVERT`。

修改 `ui_app.c` 中的界面文字后，需运行 `python tools/gen_ui_fonts.py` 重新生成字模（需联网，只提交字符本身）。

编译与烧录：在 VS Code 中使用 ESP-IDF 扩展的 Build / Flash / Monitor，或在 IDF PowerShell 中执行 `idf.py -p COMx flash monitor`。

## 安全提示

850 nm 红外输出不适合用肉眼判断亮灭或强度。调试时请使用摄像头、红外探测卡或光功率计确认输出，避免近距离直视发光面；首次通电应从限流电源、低占空比和单驱动测试开始。

## 相关资料

- [完整硬件设计报告](850nm控制验证板设计报告.md)
- `assets/850nm-control-board/`：原理图与 PCB 布局图片
