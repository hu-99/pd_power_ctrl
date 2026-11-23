# pd_power_ctrl — USB-PD 闭环恒功率电子负载控制器

[![MCU](https://img.shields.io/badge/MCU-PY32F003F18P6_(Cortex--M0%2B)-blue)]()
[![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS_v11.2.0_(Static_NoHeap)-brightgreen)]()
[![Tests](https://img.shields.io/badge/Host_Tests-25%2F25_Passed-success)]()
[![Footprint](https://img.shields.io/badge/ROM%2FRAM-34.80KB_%2F_5.87KB-orange)]()
[![Bench](https://img.shields.io/badge/Bench_Verified-20.02W_%7C_%C2%B10.02%25-red)]()
[![License](https://img.shields.io/badge/License-MIT-lightgrey)](LICENSE)

> **这是什么？**  
> 一台基于入门级 **`PY32F003`（`64KB Flash / 8KB SRAM`）** 打造的**便携式 USB-PD 协议测试 & 恒功率电子负载仪**。  
> 插上任意 Type-C PD 充电头即可自动协商电压档位、按目标瓦数闭环恒功率带载（当前电压不足时可自动逐级升档），并在 `0.96"` 赛博像素彩屏上实时显示功率、电压、电流、占空比与芯片温度，同时具备工业级多级保护与掉电黑匣子追溯。
>
> **实际用途 & 同源场景：**  
> - **研发测试工具**：验证 PD 充电头/充电宝真实 PDO 档位、满载烤机稳定性、Type-C 线缆压降与偶发保护抓拍；  
> - **量产产品原型**：与 PD 便携恒温电烙铁、迷你加热台、恒功率摄影补光灯、恒功率雾化/加热设备共享同一套「PD 协商 + 实时功率闭环 + 过温折返 + 故障黑匣子」核心控制架构。

---

## 真机一镜到底演示（`Auto-PD` 二级升压 + `vSafe5V` 安全回落）

> **演示流程（`2×` 速，全程仅串口下发目标瓦数，零手动调压）**：`5V (2W)` → 发 `P 6000` 自动跳 **`9V (6W)`** → 发 `P 20000` 自动跳 **`12V (20.3W)`** → 阶跃回落 **`5W`** → 发 `P 0` 停载自动安全退回 **`5V (vSafe5V)`**

<p align="center">
  <img src="doc/assets/bench_autopd_demo.gif" alt="PY32F003 USB-PD Auto-PD 与 vSafe5V 真机一镜到底演示" width="760"/>
</p>

### 真机特写与同步实测证据

| 0.96" 赛博 HUD 彩屏特写（`6.25KB` BC1 软解） | `9V` PD 切压与炬为表同框校验 | `12V 20.02W` 满载同框实拍 |
| :---: | :---: | :---: |
| ![0.96寸彩屏实拍](doc/assets/bench_tft_first_boot.jpg) | ![9V同框校验](doc/assets/bench_9v_live_sync.jpg) | ![20W满载同框](doc/assets/bench_20w_full_load.jpg) |
| 首版一次点亮，零全屏显存透明叠字 | 炬为 `9.007V` vs 彩屏 `09.0V` | 炬为 `1.719A / 20.02W` vs 彩屏 `1.71A / 19.4W` |

| `10W → 5W` 阶跃收敛日志（稳态 `4999~5001 mW`，`±0.02%`） | 真机 `OCP_S` 过流保护 + 双页 Flash 黑匣子异步落盘 |
| :---: | :---: |
| ![5W稳态串口日志](doc/assets/bench_uart_5w_precision.png) | ![OCP黑匣子串口日志](doc/assets/bench_uart_ocp_blackbox.png) |

**全自动 `Auto-PD` 二级升压（`5V → 9V → 12V, 20W`）+ `vSafe5V` 停机安全回落（`12V → 5V`）真机同步波形（`COM7` 实时录数）：**

![Auto-PD 与 vSafe5V 真机实测波形](doc/assets/bench_autopd_vsafe5v_waveform.png)

---

## 核心实测指标一览

| 维度 | 实测结果 | 说明 |
| :--- | :--- | :--- |
| **恒功率闭环精度** | **`±1 mW` (`±0.02%`)** | `10Ω` 实板 `5W` 稳态 `4999~5001 mW`，`10W` 稳态 `9996 mW`（`0.04%`），阶跃零超调 |
| **满载实测能力** | **`20.02 W`**（`11.74V / 1.719A`） | `10Ω // 15Ω = 6Ω` 并联负载，电流与商业炬为表偏差仅 `~0.5%` |
| **Flash / SRAM 占用** | **`34.80 KB` / `5.87 KB`** | 仅 `63.75KB / 7.97KB` 可用空间下集成全静态 FreeRTOS + 双 DMA + 彩屏 + 黑匣子 + `vSafe5V` |
| **三任务满载栈水位** | **`HWM = 62 / 80 / 68 words`** | `CTRL(160)` / `PD(160)` / `LOG(192)` 全功能满载各剩 `248B / 320B / 272B`，零堆分配 |
| **彩屏背景压缩** | **`25.0 KB → 6.25 KB` (4:1)** | 自研 M0+ 无除法 `BC1/DXT1` 软解（`PSNR 34.66 dB`），仅用 `1,280B` 单条带缓冲 |
| **自动化验证门禁** | **`25 / 25` 通过，Keil `0 Error / 0 Warning`** | PC 宿主机 MinGW GCC (`-Werror`) + Mock I2C/Flash 全覆盖（含 `Auto-PD` 与 `vSafe5V`） |

---

## 系统架构图

```mermaid
flowchart LR
    subgraph PWR["高压功率主回路 (5V ~ 20V)"]
        CHG["PD 充电头"] --> HUSB["HUSB238<br/>PD Sink (0x08)"]
        HUSB -->|"V+"| INA["INA219 / INA226<br/>R100 采压采流 (0x40)"]
        INA --> MOS["双 D4184 N-MOS<br/>20kHz 低侧斩波"]
        MOS --> LOAD["黄金铝壳负载电阻<br/>(6Ω / 10Ω / 35Ω)"]
    end

    subgraph MCU["PY32F003F18P6 (24MHz M0+ | 64KB Flash / 8KB SRAM | 静态 FreeRTOS)"]
        TCTRL["Task_Ctrl (Prio 3, 20ms)<br/>定点 PID + OVP/OCP/OTP 保护"]
        TPD["Task_PD (Prio 2, 50ms)<br/>PD 策略机 + Auto-PD 自动升挡"]
        TLOG["Task_Log (Prio 1, 50ms)<br/>串口交互 + 200ms 彩屏刷新<br/>+ 异步黑匣子落盘 + IWDG 汇总喂狗"]
        DMA1["DMA1_CH1 + ADC1<br/>内温 & VREFINT 循环采样"]
        DMA2["DMA1_CH2 + SPI1 (12MHz)<br/>1.25KB 单条带 + BC1 软解"]
        BBOX["双页 2×128B Flash 黑匣子<br/>+ 顶段 NOINIT 热复位遗言区"]
    end

    INA <-->|"I2C1 (PB6/PB7)"| TCTRL
    HUSB <-->|"I2C1 (PB6/PB7)"| TPD
    TCTRL -->|"TIM1_CH3 (PA0)"| MOS
    DMA1 --> TCTRL
    TLOG --> DMA2 --> TFT["0.96寸 BOE ST7735S<br/>160×80 横屏赛博 HUD"]
    TCTRL -.->|"故障快照"| TLOG --> BBOX
    TLOG <-->|"USART2 (PA2/PA3)"| PC["PC 串口终端"]
```

---

## 实板排错与架构权衡（Engineering Trade-offs & Troubleshooting）

| 真实工程问题 / 资源约束 | 根因定位与架构取舍 | 量化实测收益 |
| :--- | :--- | :--- |
| **`20kHz PWM` 与电流采样拍频抖动** | `10Ω` 低占空比斩波下，`INA219` 默认 `4x` 均值（`2.13ms`）与 PWM 发生采样混叠，稳态功率出现 `±200mW` 抖动。按 `20ms` PID 周期反推最大允许转换窗口，将配置改为 **`0x3D67`（分流 16x 硬件平均 `8.51ms` + 总线 4x `2.13ms` = `10.64ms < 20ms`）**。 | `5W` 稳态误差由 `±200mW` 压至 **`±1mW (±0.02%)`**，`9V` 满载 `I=805mA` 连续 20 帧零跳动。 |
| **`8KB SRAM` 放不下 `25.6KB` 彩屏显存** | `160×80` RGB565 全帧需 `25.6KB`（超芯片总 RAM 3 倍），裸图放 Flash 也要吃掉 40% ROM。采用自研 Python PCA 编码器做 **`BC1/DXT1` 4:1 压缩**，M0+ 端用 **`(x*683)>>11`** 消除 `/3` 软除法，调色板级大端交换直出 `SPI1+DMA`，配合 **`1,280B (160×4)` 单条带局部脏矩形**流式叠字。 | 背景图 **`25.0KB → 6.25KB`**（`PSNR 34.66 dB`），全彩屏子系统仅占 `1.25KB RAM`，不阻塞 `20ms` 控管。 |
| **高优先级快环禁做 Flash 擦写** | 片上 Flash 页擦写需毫秒级阻塞且暂停取指，若在 `Task_Ctrl` 直接写故障日志会拖垮闭环甚至误触 `IWDG`。采用 **快环仅立即清零 PWM + 锁存状态 + 投递快照 → `Task_Log` 异步双页乒乓落盘**（`seq + IEEE CRC32` 防掉电撕裂）。 | 故障关管响应保持单拍确定性；`OCP_S / OVP / OTP / IWDG` 实板落盘零丢帧、正常启动零磨损。 |
| **传感器型号兼容与引脚复用紧张** | `TSSOP20` 封装引出 GPIO 极少且实物可能配 `INA219`（无 `ALE` 脚、支持 `3.2A`）或 `INA226`（带 `ALE` 硬件瞬断、满量程 `819mA`）。通过 `i2c_bus_ops_t` 探测 `0xFE == 0x5449` 运行时自动识别芯片，动态切换校准公式、OCP 红线（`2.0A` vs `750mA`）与 `PA4` 报警屏蔽。 | 单一固件无缝兼容两代采样芯片，无需改板重编；剩 5 根脚刚好完整接入 `SPI1` 彩屏。 |

---

## RTOS 任务调度与 8KB SRAM 内存账

> 全系统强制 `configSUPPORT_STATIC_ALLOCATION = 1` / `configSUPPORT_DYNAMIC_ALLOCATION = 0`（`Heap_Size = 0`），开启 `configCHECK_FOR_STACK_OVERFLOW = 2` 与 `HWM` 实时水位监控。

| 任务 | 优先级 | 周期 | 静态栈分配 | 满载实测峰值 / 剩余 (`HWM`) | 核心职责 |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **`Task_Ctrl`** | `3` (最高) | `20 ms` | `160 words` (`640 B`) | `392 B` / **`248 B` (`62w`)** | `INA` 采压采流、定点恒功率 PI、软启动斜坡、OVP/OCP/OTP 瞬时关管 |
| **`Task_PD`** | `2` | `50 ms` | `160 words` (`640 B`) | `320 B` / **`320 B` (`80w`)** | `HUSB238` 6 档 PDO 择优协商、`Auto-PD` 饱和自动升挡、`vSafe5V` 停机/故障安全降压 |
| **`Task_Log`** | `1` (最低) | `50 ms` | `192 words` (`768 B`) | `496 B` / **`272 B` (`68w`)** | 串口 CLI、`200ms` 彩屏差分刷新、异步 Flash 黑匣子写盘、三任务心跳汇总喂 `IWDG` |

**`8,192 B (8.00 KB)` SRAM 精准分区：**
- **链接器保留顶段 `NOINIT` 遗言区**（`0x20001FE0..0x20001FFF`）：`32 B`（专存 `IWDG` 复位前最后一刻电压/电流/占空比/失联任务掩码，热复位不清零）
- **应用可用 `RW_IRAM1`**（`8,160 B / 7.97 KB`）→ 实占 **`6,008 B (5.87 KB)`**，余量 **`2,152 B (2.10 KB)`**：
  - 3 业务任务静态栈 + Idle 任务栈（`96w`）：`2,432 B`
  - `TFT` `160×4` 单条带 DMA 缓冲 `s_strip_buf[640]`：`1,280 B`
  - Cortex-M0+ 中断主栈 `MSP`（`0x400`）：`1,024 B`
  - FreeRTOS 内核 + 3 TCB + 3 Mutex + 黑匣子状态 + ADC/UART 缓冲：`1,272 B`

---

## 5 大核心工程亮点

1. **极限资源下的全静态 RTOS + 双通道 DMA**：在 `8KB SRAM` 上零 `malloc` 运行 FreeRTOS 三任务；`DMA1_CH1` 零 CPU 开销持续采样内部温度与 `VREFINT`（实时校正 `VDDA` 并驱动 `70~85°C` 线性过温降功率），`DMA1_CH2` 异步推送 `12MHz SPI1` 彩屏像素，完全不阻塞 `20ms` 功率闭环。
2. **软硬解耦 + PC 宿主机门禁测试**：纯 C99 业务层（`Logic_PureC/`）与外设驱动层（`ExtDev/`）通过 `i2c_bus_ops_t` / Flash 回调完全解耦，脱离 MCU 即可在 PC 端用 MinGW GCC (`-Wall -Wextra -Werror`) 跑通 `23/23` 单元与故障注入测试。
3. **`Auto-PD` 饱和升压 + `vSafe5V` 安全回落闭环**：带载占空比顶满（`>=940pm`）且欠功率持续 `1.0s` 时自动逐级升档（`5V→9V→12V→15V→20V`，内置 `I>=20mA` 空载防误升与 `1.5s` 冷却）；带载停机 (`P 0`) 等 `500ms` 电流归零后自动退回 **`5V (vSafe5V)`**、故障锁存立即降回 `5V`（同时兼容停载后手动 `V` 空载测充电头），已实板跑通 `5V→9V→12V (20W)→5V` 全流程。
4. **协作看门狗 + 防掉电撕裂双页黑匣子**：三任务心跳位图汇总由最低优先级任务喂 `IWDG`；末两页 `0x0800FF00/FF80` Flash（单调 `seq + IEEE CRC32` 双页乒乓，正常开机零磨损）配合 `NOINIT` 遗言区，真机实测 `UVP / OVP / OCP_S / OTP / IWDG` 全路径可追溯。
5. **8KB 内存无除法 `BC1` 4:1 彩屏子系统**：自研 Python PCA+最小二乘编码器 (`Tools/img2bc1.py`) 与 M0+ 无除法软解器，仅用 `1,280B` 单条带缓冲实现 `O(1)` 局部背景解码 + 透明叠字刷新。

---

## 硬件接线速查（PY32F003_Start Kit V1.1）

| 类别 | PY32F003 引脚 | 外设 / 复用 | 对接模块与说明 |
| :--- | :--- | :--- | :--- |
| **I2C 总线** | `PB6` / `PB7` | `I2C1` (AF6, 100kHz) | 并联 `HUSB238 (0x08)` + `INA219/INA226 (0x40)`（借板载 10k 上拉） |
| **PWM 功率输出** | `PA0` | `TIM1_CH3` (AF13, 20kHz) | 双 `D4184` MOS 模块 `TRIG/PWM` |
| **串口调试** | `PA2` / `PA3` | `USART2` (AF4, 115200) | `CH340` (`3.3V` 档) `RXD / TXD` |
| **0.96" TFT 彩屏** | `PA5` / `PA7` | `SPI1_SCK / MOSI` (AF0, 12MHz) | 京东方 `ST7735S` `SCL / SDA`（`DMA1_CH2` 异步发送） |
| **TFT 控制脚** | `PA1` / `PA6` / `PA12` | 推挽输出 | `RES / DC / CS`；`VCC` 与 `BLK` 接 **`3.3V`（严禁 5V）** |
| **硬件报警** | `PA4` | 上拉输入 | 仅 `INA226` 时接 `ALE`；使用 `INA219` 时悬空即可 |

> ⚠️ 完整功率主回路接线与 5 条防炸红线请先阅读 [01_硬件清单与完整接线图.md](doc/01_%E7%A1%AC%E4%BB%B6%E6%B8%85%E5%8D%95%E4%B8%8E%E5%AE%8C%E6%95%B4%E6%8E%A5%E7%BA%BF%E5%9B%BE.md)。

---

## 串口交互指令（`115200 8N1`，发送新行 `\r\n`）

| 指令 | 示例 | 功能 |
| :--- | :--- | :--- |
| `V <mV>` | `V 12000` | 设置目标 PD 电压（`5000 ~ 20000 mV`，不支持档位自动向下兼容） |
| `P <mW>` | `P 5000` | 设置恒功率目标（`0 ~ 20000 mW`，`P 0` 关闭输出） |
| `A` | `A` | 开关 `Auto-PD` 饱和自动升压挡（默认 **ON**） |
| `C` | `C` | 清除 `UVP / OVP / OCP / OTP` 故障锁存 |
| `L` / `E` | `L` / `E` | 导出 / 清空双页 Flash 故障黑匣子（最近 6 条完整现场快照） |
| `T` / `O` / `W` | `W` | 故障注入自检：`T` (`[SIM]` OVP)、`O` (`[SIM]` 90°C OTP)、`W` (IWDG 硬复位) |

---

## 仓库目录与一键构建

```text
pd_power_ctrl/
├── Logic_PureC/    # 零硬件依赖纯 C99 核心：PD 策略机/Auto-PD、定点 PID、保护状态机、双页黑匣子、BC1 软解
├── ExtDev/         # 依赖注入外设驱动：HUSB238、INA219/INA226 自动识别
├── Firmware_PY32/  # PY32F003F18P6 Keil MDK 工程（静态 FreeRTOS + PY32 LL + ST7735S HUD + bg_cyber_mech.h）
├── Tests_Host/     # PC 宿主机 MinGW GCC 单元测试（23/23 全绿）
├── Tools/          # img2bc1.py 离线 BC1 压图编码器 + 赛博 HUD 原图素材
└── doc/            # 完整图文文档与真机实测证据图集
```

```powershell
# 1. 运行 PC 宿主机全套单元测试（MinGW GCC -Werror，无需硬件）
powershell -ExecutionPolicy Bypass -File .\run_host_tests.ps1

# 2. 命令行全量编译 PY32F003 Keil 固件并输出 Flash/RAM 体积
powershell -ExecutionPolicy Bypass -File .\Firmware_PY32\build_keil.ps1 rebuild

# 3. 将任意自定义图片压成 6.25KB BC1 背景头文件并生成对比预览
python .\Tools\img2bc1.py .\Tools\assets\cyber_mech_hud_src.jpg --stretch --symbol g_bg_cyber_mech --out-c .\Firmware_PY32\Inc\bg_cyber_mech.h
```

---

## 详细技术文档与开源协议

- [01_硬件清单与完整接线图.md](doc/01_%E7%A1%AC%E4%BB%B6%E6%B8%85%E5%8D%95%E4%B8%8E%E5%AE%8C%E6%95%B4%E6%8E%A5%E7%BA%BF%E5%9B%BE.md) — 物料清单、功率/控制双回路拓扑与焊接防坑红线
- [02_软件架构与验证基线.md](doc/02_%E8%BD%AF%E4%BB%B6%E6%9E%B6%E6%9E%84%E4%B8%8E%E9%AA%8C%E8%AF%81%E5%9F%BA%E7%BA%BF.md) — 全套实测基线数据、串口证据截图与 `160×80` 叠字坐标规范
- 本项目采用 [MIT License](LICENSE) 开源（`Firmware_PY32/ThirdParty/` 下的 FreeRTOS 与 PY32 LL 驱动保留各自原开源许可）。
