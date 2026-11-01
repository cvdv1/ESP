# ESP32-S3 多波形演示 + 逻辑分析仪观测

> 工程位置：`E:\ESP\la-wave-demo`　　目标芯片：**ESP32-S3**　　框架：**ESP-IDF v5.3.5**
> 逻辑分析仪：**24 MHz / 8 通道（CY7C68013A + fx2lafw，淘宝入门款）+ Saleae Logic 2**
>
> 这个工程一次输出 **8 路互不相同的波形**，每一路演示一种"生成波形"的思路，
> 并且用一根 SYNC 做触发，让逻辑分析仪一次采集就能把全部通道抓在同一屏里。
>
> 📌 分析仪的型号细节、驱动装法、CH1~CH8↔CH0~CH7 的对应关系、以及这块板子的限制，
> 见**第五节 5.1 和 5.7**。

---

## 一、先看结果：8 个通道分别是什么

| 逻辑分析仪通道 | ESP32-S3 引脚 | 波形 | 用什么产生 | 你应该量到 |
|---|---|---|---|---|
| CH0 | GPIO4 | 1 kHz 方波，占空比 50% | LEDC（硬件 PWM） | 周期 1000 µs，高电平 500 µs |
| CH1 | GPIO5 | 10 kHz 载波，占空比 10.16%↔89.84% 每 1 s 三角扫描 | LEDC（另一个 timer） | 周期 100 µs，脉宽 10.16→89.84 µs 缓慢变化 |
| CH2 | GPIO6 | 一簇 16 个脉冲，高电平 2,3,4…17 µs，间隔 2 µs | RMT（任意脉宽引擎） | 簇长 194 µs，最短脉冲 2 µs，每 50 ms 一簇 |
| CH3 | GPIO7 | 约 250~500 kHz 方波，频率和占空比都在抖 | GPIO 软件翻转（死循环） | 每次测频率都略有不同 —— 这就是"软件时序" |
| CH4 | GPIO15 | UART1 TX，115200 8N1，文本 `ESP32-S3 LA\r\n` | UART 外设 | 空闲高电平，一帧线上 1128 µs |
| CH5 | GPIO16 | SPI2 SCLK，1 MHz | SPI 外设 | 时钟周期 1 µs，一帧 32 个时钟 |
| CH6 | GPIO17 | SPI2 MOSI，`AA 55 F0 0F` | SPI 外设 | 能被 SPI 解码器读成 4 个字节 |
| CH7 | GPIO18 | 每 50 ms 一个 10 µs 高脉冲 | GPIO | **触发用**：上升沿 = 每个采集周期的起点 |

时间关系（都相对 SYNC 的上升沿）：

```
SYNC ↑ ─┬─ +0.5 ms ──→ CH2 RMT 突发（194 µs）
        ├─ +2.0 ms ──→ CH4 UART 帧（1128 µs 线上时间）
        └─ +4.0 ms ──→ CH5/CH6 SPI 帧（4 字节 @1 MHz）
                每 50 ms 重复一次；CH0/CH1/CH3 全程连续输出
```

---

## 二、原理：ESP32 造波形有 4 条路

### 2.1 GPIO 软件翻转 —— 最灵活，也最不准

```c
gpio_set_level(pin, 1);  esp_rom_delay_us(1);
gpio_set_level(pin, 0);  esp_rom_delay_us(1);
```

- 想输出什么时序就输出什么，不占外设资源；
- 但每次 `gpio_set_level()` + 延时函数都有开销，**边沿时间由 CPU 调度决定**：
  中断、任务切换、Flash 操作都会让边沿"错位"，表现为**抖动（jitter）**；
- 实用上限大约几百 kHz；频率越高、越要"准"就越不能这么干。

**CH3 就是专门留的反面教材**：它和 CH0 都是方波，区别只在于一个是软件、一个是硬件。

### 2.2 LEDC —— 输出精确的 PWM

LEDC 是纯硬件计数器，核心公式：

```
频率 = 时钟源 / (分频系数 × 2^分辨率)
```

- 时钟源默认是 APB **80 MHz**；分频系数支持 8 bit 小数，所以很多频率能"精确"分出来；
- **分辨率越高，频率上限越低**（本工程用的两个点）：

| 分辨率 | 计数值范围 | 该分辨率下的频率上限 | 占空比步进（1 kHz 时） |
|---|---|---|---|
| 8 bit | 0~255 | 312.5 kHz | — |
| 10 bit | 0~1023 | 78.1 kHz | 0.977 µs |
| 14 bit | 0~16383 | 4.88 kHz | — |

- 一个 **timer 只能有一个频率**：本工程 CH0 用 `LEDC_TIMER_0`，CH1 用 `LEDC_TIMER_1`；
- 换频率后要重新算占空比计数值，否则占空比会跟着变；
- 8 bit 时 **100% 占空比不可达**（最大 255/256 = 99.6%），这是 LEDC 的已知限制；
- `ledc_set_duty_and_update()` 属于 **fade 服务**的一部分，**必须先调 `ledc_fade_func_install(0)`**，
  否则运行时会返回 `ESP_FAIL` 并打印 `Fade service not installed`（本工程第一次上板就踩了这个坑，
  `ESP_ERROR_CHECK` 直接 abort → 板子反复重启）。

### 2.3 RMT —— 想输出任意波形就用它

RMT 里存的不是"频率"，而是一张**符号表**：每个符号 = 电平 + 持续多少个 tick。

```
tick 分辨率 = resolution_hz（本工程 1 MHz → 1 tick = 1 µs）
每个符号两段：level0/duration0 + level1/duration1
```

- 天然适合 WS2812 灯带、DHT11、红外 NEC、舵机脉冲这类"几个微秒不听话"的时序；
- 由硬件按 tick 打出，**没有软件抖动**；
- ESP32-S3 有 8 个 RMT 通道，每个通道 48 个 symbol 的内存（普通模式，非 DMA）；
- 单段最长 15 bit（32767 tick），1 MHz 分辨率下约 32.7 ms，够覆盖绝大多数协议；
- 想要 12.5 ns 级精度就把 `RMT_RESOLUTION_HZ` 设成 80 MHz（tick 数相应变大，注意 32767 上限）。

**CH2 就是符号表的直接演示**：16 个符号，高电平从 2 µs 逐个加到 17 µs。

### 2.4 UART / SPI / I2C 等协议外设

它们产生的也是数字波形，但重点不是"形状"，而是**内容**：
逻辑分析仪挂上解码器，可以直接把波形翻译成字节，用来验证"我发的"和"线上跑的"是否一致。

### 2.5 关于正弦波、三角波、锯齿波（重要）

**ESP32-S3 没有 DAC**（只有经典 ESP32 和 ESP32-S2 有 2 路 8 bit DAC）。所以：

| 你想要 | 在 S3 上怎么做 | 逻辑分析仪能看到什么 |
|---|---|---|
| 方波 / PWM | LEDC、RMT | 真实的方波边沿 ✔ |
| 任意数字时序 | RMT | 真实脉宽 ✔ |
| 正弦 / 三角 / 锯齿 | PWM 调占空比 + RC 低通滤波（见附录 A） | **只能看到占空比在变，看不到"曲线"** |
| 真正的模拟波形 | 外挂 DAC 芯片 / I2S 音频 Codec | 逻辑分析仪仍只显示 0/1，要用示波器 |

> 一句话：**逻辑分析仪只记录"高于阈值 / 低于阈值"，它天生看不见模拟形状。**
> 想验证正弦，要么用示波器，要么用逻辑分析仪看"占空比包络"是否按正弦规律变化。

### 2.6 选型口诀

| 需求 | 用什么 | 原因 |
|---|---|---|
| 固定频率的方波/PWM | LEDC | 硬件、精确、省 CPU |
| 频率要变、占空比要扫 | LEDC（改 duty） | 每个周期都能改 |
| 微秒级不规则时序 | RMT | 任意脉宽、硬件输出 |
| 一次性/低频的"随便什么时序" | GPIO + 延时 | 简单，但会抖 |
| 要能解码成数据 | UART/SPI/I2C 外设 | 解码器现成 |
| 模拟波形 | S3：PWM+RC 或外挂 DAC | S3 无内置 DAC |

---

## 三、硬件接线

### 3.1 共地是第一位的

逻辑分析仪的地（黑色夹子/排线里的 GND）**必须和开发板的 GND 连在一起**。
不共地时，两边没有共同的"0 V"参考，采到的可能是全高、全低或随机的浮空噪声 ——
这是"接上了但什么都看不到"的头号原因。

### 3.2 8 根探针怎么接

| 分析仪通道 | 接到开发板 | 信号 |
|---|---|---|
| CH0 | GPIO4 | 1 kHz 方波 |
| CH1 | GPIO5 | 10 kHz 占空比扫描 |
| CH2 | GPIO6 | RMT 突发 |
| CH3 | GPIO7 | 软件方波 |
| CH4 | GPIO15 | UART TX |
| CH5 | GPIO16 | SPI SCLK |
| CH6 | GPIO17 | SPI MOSI |
| CH7 | GPIO18 | SYNC 触发 |
| GND | 板子任意 GND | **必接** |

引脚选择的原因（为什么不用别的脚）：GPIO26~32 接 SPI Flash、GPIO33~37 在八线 PSRAM 模组上被占用、
GPIO19/20 是 USB、GPIO39~42 是 JTAG、GPIO0/3/45/46 是 strapping 脚（上电会被外部电路拉一下）。
GPIO4/5/6/7/15/16/17/18 都是干净的普通 IO。

### 3.3 电平和耐压

- 本工程输出 **3.3 V 逻辑**，大多数 8 通道分析仪的输入阈值约 1.65 V，3.3 V 完全在范围内；
- **不要把 5 V 信号接进来**（除非确认分析仪输入支持 5 V）；
- 不要给这些引脚外加电压/上拉 —— 它们是输出，外部驱动会打架。

### 3.4 接线质量

- 杜邦线越短越好（≤ 10 cm 最佳），长线会在边沿上引入振铃和毛刺；
- 一根地线可以带多根信号，但信号多时建议多接几个 GND 引脚分散回流；
- 信号线不要和电机、继电器、DC-DC 电源线并排走；
- 探针要夹牢：虚接的典型现象是"偶尔正常、偶尔全是毛刺"。

### 3.5 上电顺序

先接好地、再接信号，最后给板子上电；拔的时候反过来。热插拔信号线容易在输入端打出瞬时高压。

---

## 四、编译、烧录、看日志

### 4.1 命令行

**cmd（推荐，最省事）** —— `.bat` 会留在当前 cmd 进程里，环境变量才能生效：

```bat
:: 1) 加载 ESP-IDF 环境（每个新开的窗口都要做一次 export）
cd /d E:\ESP\v5.3.5\esp-idf
export.bat
:: install.bat 只在"工具链没装过"时才需要跑一次（本机已装好，见 4.5）

:: 2) 编译本工程
cd /d E:\ESP\la-wave-demo
idf.py set-target esp32s3      :: 只需第一次（会生成 sdkconfig）
idf.py build

:: 3) 烧录 + 串口监视（把 COM8 换成你的实际串口）
idf.py -p COM8 flash monitor
```

**PowerShell** —— 必须用 `.ps1` 版本，否则环境变量只在子进程里生效：

```powershell
# 若提示"禁止运行脚本"，先执行一次：Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
cd E:\ESP\v5.3.5\esp-idf
.\export.ps1       # 每个新窗口一次（install.ps1 只需一次，本机已装好）
cd E:\ESP\la-wave-demo
idf.py set-target esp32s3
idf.py build
idf.py -p COM8 flash monitor
```

- 串口不确定时：拔掉板子看"设备管理器"里消失的是哪个 COM；
- 退出监视：`Ctrl + ]`；
- **离线装工具链**：`E:\ESP` 里已经有 `eim-gui-windows-x64.exe`（安装器）和
  `archive_v5.3.5_windows-x64.zst`（1.95 GB 离线包）。双击安装器 → 版本选 v5.3.5 →
  选"本地离线包"安装，就不用联网下载工具链了；
- 只改参数（`wave_pins.h`）时，后续只需重新 `idf.py build flash monitor`，**不用**再 `set-target`；
- 想确认参数没算错：`python tools/expected_values.py`（见 4.4）；
- 如果命令行怎么都配不好，直接用 4.2 的 VS Code 插件，它自己管理环境。

### 4.2 VS Code（ESP-IDF 插件）等价步骤

1. 打开文件夹 `E:\ESP\la-wave-demo`；
2. 左下角选目标 `esp32s3`、串口选 `COM8`；
3. 点底部状态栏的 🔨 Build → ⚡ Flash → 🖥 Monitor。

### 4.3 启动日志应该长这样

```
I (xxx) la_wave: ESP32-S3 多波形演示启动（目标：8 通道逻辑分析仪一次抓完）
I (xxx) wave_ledc: CH0 GPIO4: target 1000 Hz, 50% (10 bit), real 1000 Hz
I (xxx) wave_ledc: CH1 GPIO5: target 10000 Hz (8 bit), duty 10%..90% -> counts 26..230, real 10000 Hz
I (xxx) wave_rmt: CH2 GPIO6: 16 pulses, 2..17 us high, 2 us gap, burst ~194 us, res 1000000 Hz
I (xxx) wave_bus: CH4 GPIO15: UART1 TX, 115200 8N1
I (xxx) wave_bus: CH5/CH6 GPIO16(SCLK)/GPIO17(MOSI): SPI2 mode0, 1000000 Hz
I (xxx) la_wave: ==================== 引脚 / 波形速查 ====================
...
I (xxx) la_wave: 输出已开始。逻辑分析仪：采样率建议 12 MHz，CH7 上升沿触发，单次采集。
```

日志里 `real 1000 Hz` 是驱动**实际配出来的**频率（分频取整后的结果），它才是你该在分析仪上量到的值。

### 4.4 参数与预期值

所有可调参数都在 `main/wave_pins.h`，改完重新 `idf.py build`。
`tools/expected_values.py` 会按同样的参数算出"应该量到多少"，方便和实测对照：

```
python tools/expected_values.py
```

**改了 `wave_pins.h` 记得同步改这个脚本**，否则核对表就是错的。

还有一个不依赖 ESP-IDF 的主机侧自检（用的是真实的 `wave_pins.h`，任意 gcc 都能编）：

```bat
gcc -std=gnu17 -O2 -I main tools/host_check.c -o host_check
host_check
```

它会复算占空比扫描范围、RMT 突发时长、时基偏移，最后打印 `PASS` 或具体是哪条参数不自洽。

`wave_pins.h` 末尾还有一组**编译期断言**（`_Static_assert`）：分辨率超过 14 bit、频率超过 LEDC 上限、
RMT 符号数超过内存块、时基偏移超过周期……这类改错会直接**编译失败**并打印中文原因，
不用等烧进去在示波器前发呆。

### 4.5 本机工具链位置（已装好）

这台机器上的 ESP-IDF 工具链已经装好，**不用再跑 `install.bat`**，只要每个新终端 `export` 一次：

| 内容 | 位置 |
|---|---|
| ESP-IDF 源码 | `E:\ESP\v5.3.5\esp-idf`（v5.3.5） |
| 工具链根目录 | `C:\Users\liuzhen\.espressif`（默认 `IDF_TOOLS_PATH`） |
| 编译器 | `.espressif\tools\xtensa-esp-elf\esp-13.2.0_20250707\xtensa-esp-elf\bin\xtensa-esp32s3-elf-gcc.exe` |
| Python 环境 | `.espressif\python_env\idf5.3_py3.12_env` |
| 已验证的构建 | `idf.py build` 通过（target `esp32s3`），固件 `build\la-wave-demo.bin` 约 266 KB，编译零警告 |
| 开发板 | 串口 **COM11**（板载 FTDI FT232，`VID_0403&PID_6001`），实测 **16 MB Flash** |
| 已烧录并实测 | 固件已在板上运行，8 路全部初始化成功、无崩溃（见附录 E） |

这些工具是从本地的 `E:\ESP\archive_v5.3.5_windows-x64.zst` 离线包解出来的（`dist\` 里的官方压缩包 +
`wheels_py312\` 里的 Python 轮子），**没有联网下载**。如果哪天要重装：把该离线包的 `dist` 解到
`.espressif\dist`，再跑 `idf_tools.py install --targets=esp32s3` 和 `install-python-env`（配合 `PIP_FIND_LINKS`）。

---

## 五、逻辑分析仪设置（Logic 2）

### 5.1 这台分析仪：24 MHz / 8 通道（CY7C68013A + fx2lafw）

本文档按你实际使用的那台"淘宝 19 元入门款"来写，它的特征：

| 项目 | 值 |
|---|---|
| 主控 | **CY7C68013A（FX2LP）** + fx2lafw 开源固件（和开源项目 miniLA 同方案） |
| 通道 | 8 通道，板上丝印 **CH1~CH8** 对应软件里的 **CH0~CH7**（⚠️ 差一位，接错就是"没波形"的常见原因） |
| 最高采样率 | **24 MHz**（超过 10 MHz 后对线材质量敏感，建议用 12 MHz） |
| 带宽/缓冲 | 无外部大缓存，采集窗口越长、采样率越高，越容易受 USB 传输限制 |
| 软件 | **Logic 2**（本机已装在 `E:\Logic\Logic.exe`）；备用 Logic 1.2.17、PulseView(sigrok) |
| 驱动 | 商家资料里的 `Logic Setup 1.2.17.exe`，或 Logic 2 自带的 `E:\Logic\Drivers\Saleae.inf`；用 PulseView 则用 **zadig** 装 libusb |

> 结论：**它完全够用**——本工程最快需要 12 MS/s，它的上限是 24 MS/s。
> 只要记住三件事：**CH1~CH8 = CH0~CH7**、**共地**、**采样率别超过 12 MHz**。
>
> 参考资料：卖家说明文档、[24MHz 逻辑分析仪使用笔记（CSDN）](https://blog.csdn.net/weixin_40289409/article/details/139343384)、
> 以及同方案的开源硬件项目 [miniLA（Gitee）](https://gitee.com/SAST-E/miniLA)。

### 5.2 新建采集

1. 插上分析仪（Win 设备管理器应出现 USB 设备，非黄色感叹号）；
2. 打开 `E:\Logic\Logic.exe`，左下角自动识别设备（常见显示为 `Logic 8` / `Saleae Logic 8`）；
3. 勾选 CH0~CH7，接好 8 根信号 + GND（见第三节）；
4. 通道改名（`1k_SQUARE`、`PWM_SWEEP`、`RMT`、`BITBANG`、`UART_TX`、`SPI_SCLK`、`SPI_MOSI`、`SYNC`），读图效率会高很多。

### 5.3 采样率：宁高勿低

**采样率必须远高于信号里"最快的那个特征"**，而不仅仅是基频。经验法则：

- 至少是最高频率成分的 **8~10 倍**；
- 同时至少让**最短脉冲**占到 **4~8 个采样点**。

本工程的推荐值：

| 通道 | 最快特征 | 最低可接受采样率 | 建议 |
|---|---|---|---|
| CH0 1 kHz 方波 | 0.5 ms 半周期 | 100 kS/s | 1 MS/s |
| CH1 10 kHz PWM | 10 µs 窄脉冲 | 500 kS/s | 2 MS/s |
| CH2 RMT | **2 µs 脉冲** | 4 MS/s | 12 MS/s |
| CH3 软件方波 | ~0.3 MHz | 3 MS/s | 12 MS/s |
| CH4 UART | 8.68 µs/bit | 1 MS/s | 2 MS/s |
| CH5/CH6 SPI | 1 µs 时钟周期 | 10 MS/s | 12 MS/s |
| CH7 SYNC | 10 µs 脉冲 | 400 kS/s | 2 MS/s |

**一次抓全所有通道：用 12 MS/s。**
你这台标称 24 MHz，但 CY7C68013A 在 24 MHz 下数字通道容易丢点（尤其杜邦线较长时），12 MHz 更稳。
如果软件只给 8 / 16 / 24 MHz 这类固定档位，选 16 MHz 或 8 MHz 都行（8 MHz 时 2 µs 脉冲只剩 16 个点，仍可分辨）。

### 5.4 采集窗口 = 采样点数 ÷ 采样率

| 采样深度 | @12 MS/s 的时间窗口 | 够不够看一个 50 ms 周期 |
|---|---|---|
| 600 k | 50 ms | 刚好一个周期（最少要这么多） |
| 1 M | 83 ms | 够（约 1.6 个周期）✔ |
| 2 M | 167 ms | 够，能看到 3 个周期 |
| 4 M | 333 ms | 够，CH1 的占空比变化看得很清楚 |

想观察 CH1 的 1 s 三角扫描，就用"低采样 + 大深度"（例如 4 MS/s × 4 M = 1 s）。

> 注意：这台 CY7C68013A 没有大缓存，**深度上限取决于设备缓冲**。Logic 2 里如果"采样时间"
> 选项有限，就按 `时间 × 采样率 ≤ 设备缓冲` 反推：本工程一屏只需 50 ms，12 MS/s 下 = 600 k 点，
> 大多数克隆版都能满足；不够就把采样率降到 8 MS/s（2 µs 脉冲仍有 16 个点）。

### 5.5 阈值与通道命名

- 阈值用默认的 **1.65 V**（3.3 V 逻辑的一半）即可，除非你的板子不是 3.3 V；
- 通道改名在 5.2 第 4 步做过；另外建议把 GND 那根线用**黑色杜邦线**单独记牢。

### 5.6 触发：用 SYNC，不要用自动

- **触发通道**：CH7（SYNC）
- **触发类型**：上升沿（Rising edge）—— 这块 CY7C68013A 板子支持边沿触发
- **触发模式**：**单次（Single）**，或者"正常（Normal）"
- 效果：每次采集都从"同一时刻"开始，CH2/CH4/CH5/CH6 的突发位置固定，多通道才能对齐比较。

第一次接好线、不确定有没有信号时，先用**连续/Live 模式**看一眼有没有边沿，再切单次触发。

> 顺带一个和资料里一致的技巧：UART 空闲是高电平、起始位拉低，所以**只看 UART** 时用**下降沿触发**更好抓；
> 而本工程要一屏抓全 8 路，就用 CH7 的**上升沿**。

### 5.7 驱动与软件（这台设备的正确装法）

**用 Logic 2（本机已装 `E:\Logic\Logic.exe`）**

1. 驱动：商家资料里的 `Logic Setup 1.2.17.exe`（推荐，最稳），或设备管理器里给"未知设备"手动指定
   `E:\Logic\Drivers\Saleae.inf`；
2. 插上分析仪 → 打开 Logic.exe → 左下角应出现 `Logic 8`；
3. 若 Logic 2 识别不到设备：说明你这个版本对克隆硬件的支持有变，换商家包里的
   **Saleae Logic 1.2.17**（老界面，功能完全够用），或走下面的 PulseView。

**用 PulseView（sigrok）** —— Logic 2 分析器受限时的最佳替代

1. 用 **zadig** 把分析仪接口的驱动换成 **libusb-win32 / WinUSB**（这一步会替换掉 Saleae 驱动，
   两者不能同时用；换回来就在设备管理器里重装 `Saleae.inf`）；
2. PulseView 里选 `fx2lafw` 驱动 → 采样率 12 MHz → 通道 D0~D7 对应本工程 CH0~CH7；
3. 解码器在 `Add protocol decoder` 里选 `UART` / `SPI`，参数与第 7 节一致。

**这块板子的现实限制（写清楚免得踩坑）**

- 24 MS/s 是标称上限，实际用 12 MS/s 最稳；**不要用 24 MS/s 抓长窗口**，容易丢点；
- 没有大缓存：采样率 × 时长 = 采样点数，超过设备缓冲就会自动截断（本工程 12 MS/s × 50 ms = 600 k 点，一般没问题）；
- 通道间没有独立阈值，全部共用 1.65 V 左右；
- 板上丝印 CH1~CH8 与软件 CH0~CH7 **差一位**，接错会"整路无波形"。

---

## 六、逐通道验证：应该看到什么、怎么量

### CH0 —— 硬件 PWM 的"标准答案"

- 采样率 1 MS/s、窗口 83 ms；
- Logic 2 里对 CH0 用 **Measurement → Frequency / Duty cycle / Pulse width**；
- 预期：**1000.0 Hz**、**50.0%**、高电平 **500 µs**；
- 83 ms 窗口里应该有 **83 个周期**；如果周期数明显不对，先检查采样率设置。

### CH1 —— 占空比在"呼吸"

- 采样率 12 MS/s、窗口 83 ms 时能看到 83 个 100 µs 周期；
- 同一屏里**每个周期的脉宽略有不同**（因为占空比每 10 ms 才变一次，一屏 83 ms 内只走了 8 步）；
- 想要"看到扫描全貌"：把采样率降到 4 MS/s、深度加到 4 M（1 s 窗口），
  就会看到一列脉冲的宽度从 10 µs 连续变宽到 90 µs 再变窄 —— 这就是**占空比包络**；
- 量某一时刻的占空比：预期在 **10.16% ~ 89.84%** 之间（计数值 26~230 / 256）；
- 每一步（10 ms）占空比变化约 **2%（4 个计数值）**，所以放大看是"阶梯式"变化，
  而不是连续直线 —— 这是数字 PWM 的离散本质，不是波形坏了。

### CH2 —— 微秒级精度的"梳子"

- 采样率至少 4 MS/s（2 µs 脉宽只有 8 个点），建议 12 MS/s（24 个点）；
- 触发在 SYNC 后 **约 500 µs** 处出现第一簇；
- 簇内 **16 个高电平**，宽度依次 **2,3,4,…,17 µs**，相邻脉冲间隔 **2 µs**；
- 整簇时长 **约 194 µs**；每 **50 ms** 重复一次；
- 用 Measurement 量每个高电平脉宽，误差应在 ±1 个 tick（1 µs 分辨率）以内。
- 如果只看到"一个宽脉冲"：说明采样率太低，把 2 µs 和 2 µs 的间隔连成一片了。

### CH3 —— 软件时序的"抖动"长这样

- 采样率 12 MS/s；
- 频率大致在 250~500 kHz，**但每次测量结果都不一样**，占空比也不是精确的 50%；
- 把 CH0 和 CH3 同时放大对比：CH0 的边沿每次都落在同一个位置，CH3 的边沿左右漂移；
- 这一路是"死循环"任务，CPU1 会被它占满（这也是为什么关了 CPU1 的 idle 看门狗，见附录 B）。

### CH4 —— 让解码器读出文本

- 采样率 2 MS/s 以上（每 bit 至少 17 个采样点）；
- 空闲时线上是**高**，每个字节以起始位（拉低）开始；
- 挂 **Async Serial** 解码器（参数见 7.1），应该解出 `ESP32-S3 LA`；
- 一帧线上时长 ≈ **1128 µs**（13 个字符 × 86.81 µs）。

### CH5 / CH6 —— SPI 时钟与数据

- 采样率 12 MS/s（1 µs 时钟周期 = 12 个点）；
- SCLK 空闲低电平（mode 0），每个字节 8 个时钟，一帧 4 字节 = **32 个时钟**；
- MOSI 上依次是 `AA 55 F0 0F`（MSB first）：`10101010 01010101 11110000 00001111`；
- 挂 **SPI** 解码器（参数见 7.2）应直接显示这 4 个字节；
- 一帧传输时间 ≈ **32 µs**（不含帧间空隙）。

### CH7 —— 触发基准

- 每 50 ms 一个 **10 µs** 高脉冲；
- 作为触发源时，"采集起点"就落在这个上升沿；
- 如果你想确认时基有没有飘：量 10 个脉冲的间距，应该是 500 ms ± 晶振误差（几十 ppm 量级）。

---

## 七、挂解码器

### 7.1 异步串口（Async Serial）

| 参数 | 值 | 说明 |
|---|---|---|
| 波特率 | 115200 | 与 `UART_BAUD_RATE` 一致 |
| 数据位 | 8 | |
| 校验 | None | |
| 停止位 | 1 | |
| 位序 | LSB first | UART 标准，不要选 MSB |
| 反相 | No | 空闲高电平就是正常 UART |
| 采样点 | 每 bit 中间 | 默认即可 |

预期输出：`ESP32-S3 LA`（`\r\n` 在部分解码器里不显示）。

### 7.2 SPI

| 参数 | 值 | 说明 |
|---|---|---|
| 时钟 | CH5（SPI_SCLK） | |
| 数据 | CH6（SPI_MOSI） | |
| CPOL / CPHA | 0 / 0（mode 0） | 与固件 `.mode = 0` 一致 |
| 位序 | MSB first | SPI 标准 |
| 每帧位数 | 8 | |
| CS | 不使用 | 固件里 `spics_io_num = -1` |

预期输出：`AA 55 F0 0F`。

> 没有 CS 时，某些解码器需要"按位数/按空格切帧"。如果显示成连续比特流，
> 可以把解码器的 "bits per transfer" 固定为 8，或按 32 位一次查看。

### 7.3 解码不对怎么排查

| 现象 | 优先检查 |
|---|---|
| 完全解不出 | 通道选错、共地、采样率低于 4×波特率 |
| 字节错位/乱码 | 位序（LSB/MSB）、波特率、CPOL/CPHA |
| 偶发错字节 | 采样率太低、线太长、阈值偏高/偏低 |
| 只有一部分 | 采集窗口太短，帧被截断 |

---

## 八、排错表

| 现象 | 可能原因 | 处理 |
|---|---|---|
| 某通道一直高/一直低 | 探针接错脚、没共地、引脚被别的功能占用 | 对照第三节表格；万用表量通断 |
| 所有通道都没信号 | 固件没跑起来 / 串口日志有 panic | 先看 `idf.py monitor` 输出 |
| 有信号但边沿很脏、到处是毛刺 | 地线太长、杜邦线太长、附近有开关电源 | 换短线、多接地、远离干扰源 |
| 频率量出来正好是一半/两倍 | 采样率不足（混叠）或测量窗口没对齐 | 提高采样率到 12 MS/s |
| 2 µs 的脉冲看不到 | 采样率 < 2 MS/s | 用 12 MS/s |
| 触发不上 | 触发通道/边沿选错、SYNC 没接、用了自动模式 | CH7 + 上升沿 + 单次 |
| UART 解出来是乱码 | 波特率/位序错，或接的是 UART0 控制台脚 | 确认接 GPIO15，参数按 7.1 |
| 波形正常但任务看门狗打印告警 | CH3 死循环占满 CPU1 | 已由 `sdkconfig.defaults` 关闭 CPU1 idle 监视，见附录 B |
| 串口反复打印 `Fade service not installed` / 板子不停重启 | 用了 `ledc_set_duty_and_update()` 但没装 fade 服务 | 调一次 `ledc_fade_func_install(0)`（本工程已修，见 2.2） |
| 启动日志 `Detected size(16384k) larger than ... (2048k)` | 板子是 16 MB Flash，固件按 2 MB 编的 | 已把 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` 写进 `sdkconfig.defaults` |
| 烧录失败 | 串口被 monitor 占用 / 板子没进下载模式 | 关掉监视、按住 BOOT 再点复位 |

---

## 九、改参数速查（都在 `main/wave_pins.h`）

| 想改什么 | 改哪个宏 | 注意 |
|---|---|---|
| CH0 频率 / 占空比 | `SQUARE_FREQ_HZ`、`SQUARE_DUTY_BITS`、`wave_ledc.c` 里的 `max_count/2+1` | 频率别超过 80 MHz / 2^分辨率 |
| CH1 频率 / 扫描范围 | `PWM_FREQ_HZ`、`PWM_DUTY_MIN_PCT/MAX_PCT` | 一个 timer 一个频率，改了频率要重新算占空比 |
| CH1 扫描速度 | `PWM_SWEEP_STEP_MS`、`PWM_SWEEP_STEPS` | 周期 = 步数 × 步长 |
| CH2 脉冲数 / 步进 | `RMT_PULSE_NUM`、`RMT_FIRST_PULSE_US`、`RMT_STEP_US` | 符号数不能超过 `RMT_MEM_BLOCK_SYMBOLS`（48），超了要开 DMA 或增大 block |
| CH2 精度 | `RMT_RESOLUTION_HZ` | 80 MHz → 12.5 ns；但单段最多 32767 tick |
| CH4 波特率 / 文本 | `UART_BAUD_RATE`、`wave_bus.c` 里的 `msg` | 改完解码器参数要同步改 |
| CH5/CH6 速度 / 数据 | `SPI_CLOCK_HZ`、`wave_bus.c` 里的 `payload` | |
| 时基周期 / 偏移 | `BURST_PERIOD_MS`、`BURST_*_US`、`SYNC_PULSE_US` | 偏移增大时注意别超过周期 |
| 引脚 | `WAVE_PIN_*` | 避开 26~32、33~37、19/20、39~42、0/3/45/46 |

改完执行：

```bat
idf.py build flash monitor
python tools/expected_values.py    :: 同步核对表数字
```

---

## 附录 A：在 ESP32-S3 上得到"看起来是模拟"的波形

S3 没有 DAC，但可以用 **PWM + RC 低通滤波** 逼近任意低频波形：

```
GPIO(PWM) ── R ──┬── R ──┬──→ 输出（接示波器/运放）
                 C       C
                 │       │
                GND     GND
```

- 截止频率 `fc = 1 / (2πRC)`；
- 例：1 kHz 正弦，用 **20 kHz 以上**的 PWM 载波，`R=1 kΩ, C=100 nF`（fc ≈ 1.6 kHz，两级 ≈ -32 dB @20 kHz）；
- 载波频率至少是目标信号频率的 **20 倍**，滤波阶数越高越平滑；
- 输出幅度 = 3.3 V × 占空比，所以直接给 DAC 值（0~255）映射成占空比就能"画"正弦；
- **逻辑分析仪上只能看到 PWM 的占空比包络**（高低电平在快速翻转，看起来是一条粗带）；
  想看真正的曲线必须用示波器 —— 这也是逻辑分析仪和示波器的根本分工。

> 如果你同时有经典 ESP32（不是 S3），它内置 2 路 8 bit DAC，可以直接 `dac_output_voltage()` 输出正弦表，
> 那才是"真模拟"。S3 只能走上面这条路。

## 附录 B：为什么 CH3 会占满 CPU1，以及看门狗配置

CH3 的 `bitbang_task` 是**故意**的死循环（没有 `vTaskDelay`），这样频率才高、抖动才明显。
副作用：CPU1 的 idle 任务长时间得不到调度，Task Watchdog 默认会每 5 秒打印一次
`task_wdt: Task watchdog got triggered`（默认不重启，但日志会被刷屏）。

因此 `sdkconfig.defaults` 里加了：

```ini
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n
```

这不会掩盖真正的死锁：CPU0 的 idle 监视仍然开着，时基任务和扫描任务若有问题照样会报。
如果你把 CH3 改成 `vTaskDelay` 版本，可以把这一行删掉。

## 附录 C：文件清单

```
la-wave-demo/
├── CMakeLists.txt            工程入口
├── sdkconfig.defaults        FreeRTOS 节拍 1 kHz、CPU1 idle 看门狗、日志等级
├── main/
│   ├── CMakeLists.txt        源文件与 REQUIRES 声明
│   ├── wave_pins.h           ★ 所有引脚和参数都在这里
│   ├── main.c                app_main、50 ms 时基任务、bit-bang 任务
│   ├── wave_ledc.c/.h        CH0 方波 + CH1 占空比扫描
│   ├── wave_rmt.c/.h         CH2 任意脉宽突发
│   └── wave_bus.c/.h         CH4 UART 帧 + CH5/CH6 SPI 帧
└── tools/
    ├── expected_values.py    按参数推算"应该量到多少"
    └── host_check.c          主机侧算术自检（任意 gcc 编译，打印 PASS/FAIL）
```

## 附录 D：可以继续做的练习

1. 把 CH2 换成 **WS2812 灯带时序**（RMT 的经典用法），再用分析仪解码颜色；
2. 用 RMT 复刻 **DHT11 单总线**的应答时序，和真实传感器的波形对比；
3. 把 CH1 的三角扫描改成**正弦表**，配合 RC 滤波后用示波器看形状；
4. 把 CH4 换成 **I2C** 主机，用分析仪解码地址+数据；
5. 把 CH3 改成"用 GPTimer 中断里翻转 GPIO"，观察抖动从 µs 级降到百 ns 级 ——
   说明"越靠近硬件，时序越准"。

## 附录 E：这个工程的验证状态（请先读）

**已经验证过的：**

- **真实编译通过**：ESP-IDF v5.3.5 + `xtensa-esp32s3-elf-gcc 13.2.0`，
  `idf.py set-target esp32s3` + `idf.py build` 返回 0，4 个源文件**零警告**，
  产物 `build\la-wave-demo.bin` 约 266 KB（app 分区剩余 74%）；
- **已在真板子上运行**：烧录到 COM11（ESP32-S3，16 MB Flash）后，启动日志 8 路全部初始化成功，
  `1000 Hz / 10000 Hz` 与驱动回报一致，连续运行无 panic、无重启（修掉 fade 服务那个 bug 之后）；
- 4 个源文件另用宿主机 gcc（`-std=gnu17 -Wall -Wextra -Wformat=2`）做过语法/类型/格式串检查：
  检查用的桩头文件是按本机 `E:\ESP\v5.3.5\esp-idf` 里 LEDC / RMT / UART / SPI / GPIO 的**真实签名**
  写的，所以字段名、参数个数、枚举名与 IDF v5.3.5 对齐；
- `wave_pins.h` 里的编译期断言全部成立；
- `tools/expected_values.py` 和 `tools/host_check.c`（后者已在主机上编译运行并打印 `PASS`）验证了算术：
  LEDC 分频系数（78.125 / 31.25，可精确表示）、占空比扫描确实走到 10.16%~89.84% 且上下坡单调、
  RMT 突发 194 µs、时基偏移 0.5 / 2.0 / 4.0 ms。

**没有验证过的（重要）：**

- **没有实物实测**：第六节的频率/占空比/脉宽都是"按参数应当测到"的期望值，不是实测记录。
  实测值与期望值不一致时，优先怀疑采样率设置与接线（见第五、八节），其次怀疑参数被改过。

**怎么快速确认固件真的在跑：**

烧录后看串口日志最后一行 `输出已开始。...`，再用逻辑分析仪只接 CH0 + GND：

- 量到 **1 kHz / 50%** → 固件、时钟、引脚都正常，可以继续接其他通道；
- 量不到 → 先解决基础问题（串口有没有 panic、探针夹的是不是 GPIO4），别急着接 8 根线。

