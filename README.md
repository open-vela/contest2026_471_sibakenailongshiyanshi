# GD32H759IMT6 openvela（NuttX）硬件适配 — 赛题 471

## 一、作品简介

在慧勤智远 GD32H759IMT6（V1.3 小系统板，Cortex-M7 @ 600MHz）上完成 **openvela（NuttX 变体）的硬件适配与工业触控可视化系统**：跑通外部 SDRAM（W9825G6KH）、板上 LED、按键、GT911/GT1158 电容触摸、TLI 并行 RGB LCD（800x480 WKS43WV067），并基于 framebuffer 自绘一套 **白底 GUI 可视化系统**（主界面 4 入口：触摸跟随 / 手写识别 / 测量控制 / 系统信息）。测量控制页内置 **ADC 采集 + FFT 频谱分析**（ADC0 14bit 软件触发采样 256 点，时域波形 / 128 条频谱 / 主峰频率，外部模拟信号接 PA4 = ADC0_CH18）与 **红/绿 LED 控制 + PWM 亮度调节**（TIMER1 软件 PWM 驱动红灯 PC13，滑块实时调占空比）。主界面集成五方合作方标识（武汉理工大学 / 小米 / openvela / 兆易创新 / 火花实验室）。

## 二、选题方向

**新硬件适配**。将 openvela（NuttX）完整移植到国产 GD32H759IMT6 微控制器平台，覆盖 BSP、外设驱动、NSH 基础运行环境、板级使能（bringup）与工业触控上层应用。

## 三、目录结构

- `board/contest_board/` — 板级适配代码
  - `src/` — board 层源码：`gd32h7xx_bringup.c`（含按需 `board_lcd_enable`）、`gd32h7xx_gt911.c`（软 I2C 触摸驱动，v49 稳定解码版）、`gd32h7xx_userleds.c`（LED，已静默化）
  - `nuttx/arch/arm/src/gd32h7xx/` — 内核驱动改动：`gd32h7xx_tli.c/.h`（新增 TLI 显示驱动）、`gd32h7xx_sdram.c`（EXMC SDRAM，SDCTL=0x59d9 消除伴线伪影）、`gd32h7xx_gpio.h`（GD32H759 端口编码修复）
  - `configs/nsh/defconfig` — 板级最小 NSH defconfig（含 GUI/LCD/触摸等配置开关）
- `app/gt911_demo/` — GT911 触摸检测示例（`gt911` 命令）
- `app/lcd_demo/` — 运行时开启 LCD 示例（`lcd` 命令，boot 不初始化、按需使能）
- `app/leds_demo/` — LED 示例（`leds` 命令，完全静默）
- `app/gui/` — GUI 可视化系统（`gui` 命令）：主界面 4 入口（触摸跟随 / 手写识别 / 测量控制 / 系统信息），自绘白底 UI + 微软雅黑风格字库 + 触摸校准（9 点）+ 五方合作 logo
- `firmware/` — 已交付固件（bin），版本说明见下
- `logs/` — AI Coding 日志（持续更新）

## 四、运行方式

1. 按 README 顶部流程 `repo init` / `repo sync` 拉取 openvela 全量源码与本仓。
2. 将本仓代码应用到对应路径（均为增量改动）：
   - `board/contest_board/src/*` → `vendor/gigadevice/boards/gd32h7/gd32h759imt6/src/`
   - `board/contest_board/nuttx/arch/arm/src/gd32h7xx/*` → `nuttx/arch/arm/src/gd32h7xx/`
   - `board/contest_board/configs/nsh/defconfig` → `vendor/gigadevice/boards/gd32h7/gd32h759imt6/configs/nsh/defconfig`
   - `app/gt911_demo/*` → `apps/examples/gt911/`；`app/lcd_demo/*` → `apps/examples/lcd/`；`app/leds_demo/leds_main.c` → `apps/examples/leds/`；`app/gui/*` → `apps/examples/gui/`
3. 启用相关配置：`CONFIG_GD32H759IMT6_GT911`、`CONFIG_EXAMPLES_GT911`、`CONFIG_EXAMPLES_LCD`、`CONFIG_EXAMPLES_GUI`、`CONFIG_GD32H7_TLI`。
4. 编译：`./build.sh vendor/gigadevice/boards/gd32h7/gd32h759imt6/configs/nsh/defconfig -j8`。
5. 串口 115200 进入 NSH：
   - `leds` — LED 闪烁（无串口输出）
   - `gt911` — 触摸检测
   - `lcd` — 按需开启 LCD 并画测试图案
   - `gui` — 启动可视化系统（触摸校准后进入主界面；测量控制页自动运行 ADC 采集 + FFT 频谱，可控制 LED 与 PWM 亮度）

## 五、固件版本说明（firmware/）

| 版本 | 说明 | 状态 |
|---|---|---|
| `nuttx_v49.bin` | 稳定基线：3 按钮 GUI + 触摸可用 | ✅ 用户确认 |
| `nuttx_v55.bin` | 首个 ADC 采集 + FFT 频谱版 | ⚠️ 采样链路未打通，无波形 |
| `nuttx_v56.bin` | v55 + 采样范围统计/寄存器回读诊断 | ⚠️ 定位用 |
| `nuttx_v57.bin` | v56 + EOC 等待超时保护（不卡死）+ 诊断前置 | ⚠️ 定位用 |
| `nuttx_v58.bin` | v57 + DAC→ADC 回环探针（定位用） | 🔧 调试中（已弃用 DAC 方案） |
| `nuttx_v62.bin` | 通道扫描诊断（CH0-4/CH18）→ 证明 ADC 转换/参考正常 | ✅ 定位完成 |
| `nuttx_v64.bin` | **修复主采样未调用 bug**，首个采到真实数据（PA4 悬浮噪声 4628~7140） | ✅ 里程碑 |
| `nuttx_v65.bin` | 每帧保持 ADC0 时钟（修复后续帧全 0） | ✅ 修复 |
| `nuttx_v66.bin` | ADC+FFT 整理版（清理全部调试输出） | ✅ 完赛版 |
| `nuttx_v67.bin` | GUI 主界面扩为 4 入口（+手写识别），手写识别改用自有数据（0-9 各 10 样本训练） | ✅ 用户确认 |
| `nuttx_v72.bin` | 测量控制页红/绿 LED 亮灭控制（PC13 红 / PJ8 绿，寄存器直控） | ✅ 用户确认 |
| `nuttx_v75.bin` | ADC 静态数据 + FFT 频谱（去抖动化），触摸改为边沿触发（修复多次触发） | ✅ |
| `nuttx_v77.bin` | 波形越界钳制 + 面板标签（时域波形 / FFT 频谱） | ✅ |
| `nuttx_v78.bin` | 主界面五方合作 logo（武汉理工 / 小米 / openvela / 兆易创新 / 火花实验室） | ✅ 用户确认 |
| `nuttx_v86.bin` | 系统信息页兆易创新图标（标题下方居中） | ✅ |
| `nuttx_v87.bin` | 测量控制页 PWM 输出 + 占空比滑块（软件 PWM：TIMER1 中断翻转 PC13） | ✅ 用户确认 |
| `nuttx_v89.bin` | 修复 PWM RCU 基地址总线错误（v88 呼吸灯方案被用户否决不收录） | ✅ 修复 |
| `nuttx_v90.bin` | **PWM 固定占空比调亮度（当前推荐）**：滑块实时调红灯亮度，0 灭 / 100 全亮 | ✅ 完赛版 |

中间版本 v50-v54（触摸大端解码尝试、手写识别调整）均被实测否定后回退；v59-v61/v63 为 ADC 中间排障版（DAC 移除、复位修正、VREF 使能尝试）；v68-v71/v73-v76/v79-v85 为 GUI/logo/LED 迭代细节，均已被表中关键版本取代。

### ADC 频谱功能

- 采集：ADC0（0x40012400）14bit，通道 18 = PA4（数据手册确认 ADC01_IN18），软件触发单次转换，256 点
- 显示：左半屏时域波形，右半屏 FFT 频谱 128 条，右上角主峰频率（显示采样率 36363 Hz）
- 寄存器直操实现（参照官方例程 18），不依赖标准库
- 排障结论：v62 通道扫描证明 ADC 转换链路与参考正常（CH2/3/4/18 均采到有效值）；v64 定位根因为主采样函数未被调用；v65 修复采样间 ADC0 时钟被清问题

### PWM 亮度调节

- 红灯（LED0=PC13）无定时器通道复用功能（GPIO/RTC only），采用 **TIMER1 更新中断软件 PWM**：TIMER1（0x40000000，APB1）以约 10 kHz 更新率中断，按占空比翻转 PC13，形成 100 Hz PWM
- 拖动滑块实时调整占空比（0-100%）：占空比越高红灯越亮，0 灭、100 全亮；绿灯仍为普通开关（PJ8）
- 关键排障：v88 曾实现呼吸渐变被用户否决（仅需调亮度）；v89 修复 RCU 基地址总线错误（APB1EN 正确地址 0x58024440，原误写 0x58024040 导致进测量页 Bus Fault）

## 六、AI Coding 使用说明

全程使用 AI 助手（豆包）辅助：BSP 移植方案设计、TLI 驱动编写与寄存器级排查、软 I2C 时序打通、GT911 坐标解析（逐字节比对例程与 Linux goodix.c）、LCD 伴线伪影定位（SDRAM 时序）、GUI 字库构建与触摸校准、ADC 寄存器直操与 FFT 频谱实现、PWM 软件实现与总线错误排障。AI 在驱动调试（位域推算、时序窗口分析）与代码量产出上显著提升了效率。完整对话日志见 `logs/`，并沉淀可复用 Skill（`skills/gd32h759-vm-build`：VM 编译-上传-拉取固件闭环）。

## 当前状态

- ✅ 稳定：外部 SDRAM、userleds（红/绿灯）、buttons、GT911 触摸（v49 解码基线）、TLI LCD 显示（800x480 RGB565，无伴线伪影）、白底 GUI（4 入口 + 触摸校准 + 微软雅黑字库 + 五方合作 logo）
- ✅ 已实现：ADC 采集（ADC0 CH18/PA4，14bit 软件触发）+ FFT 频谱显示（时域波形 / 128 条频谱 / 主峰频率）、手写数字识别（自有数据 0-9 各 10 样本）、LED 亮灭控制、PWM 亮度滑块
- 🔧 历史排障：热复位卡死（面板复位忙等 → 按需初始化）、LED 刷屏、触摸初始化慢、LCD 伴线（SDRAM 时序）、ADC 采样恒 0（主采样未调用 + 时钟被清）、PWM 总线错误（RCU 基址）均已定位并修复
