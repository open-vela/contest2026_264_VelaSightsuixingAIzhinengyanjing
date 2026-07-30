# BK7258 Camera（GC2145）与 LCD（GC9D01）驱动移植设计

状态：已批准（方案 A），待写实施计划

## 1. 背景

比赛仓 `contest2026_264_VelaSightsuixingAIzhinengyanjing` 目前已完成 OpenVela/NuttX 在 BK7258
物理 CPU1（AP primary core）上的裸机 bring-up：启动向量、C runtime、MPU/FPU/IRQ/SysTick、
Mailbox v2 基础传输、PWC 握手、AP 日志经 Mailbox 转发到 CP UART0。详见仓库根 `README.md` 与
`.repo/manifests/github开发与构建指南.md`；移植依据见飞书文档
[VELA大赛文档](https://mi.feishu.cn/wiki/HBLbwAcNAiq3X3kVsDtc59Whnqf) 下的
`BK7258_OPENVELA_AP_PORTING_PLAN`。

当前实现范围明确排除了摄像头和图形/显示驱动（PORTING_PLAN 第 10 节：不启动任何要求 CPU2、
Wi-Fi、BT、媒体或 AP SMP 的服务）。本设计文档补上这一块：让 OpenVela AP 侧具备最基础的
摄像头采集能力和屏幕显示能力，作为后续多媒体/AI 功能的地基。

## 2. 硬件事实（已从代码交叉验证）

板载外设（源自 `bk_solution_ai` AI 解决方案简介文档"完整的外设支持"一节，并与
`bk_avdk_smp` release/v3.1.1 驱动源码交叉核对）：

- **摄像头**：GC2145，**DVP 并口**（8-bit 并行数据 + PCLK/VSYNC/HSYNC），控制走 I2C
  （地址 `0x78` 写 / `0x79` 读），输出格式 YUV422（也支持后端硬编码 MJPEG/H.264，本阶段不用）。
  驱动依据：`bk_avdk_smp/ap/components/bk_peripheral/src/dvp/dvp_gc2145.c`。
- **屏幕**：GC9D01，双屏，**QSPI** 接口（60 MHz），分辨率 **160×160**，RGB565。
  驱动依据：`bk_avdk_smp/ap/components/bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c`
  （文件路径写作 spi，但初始化命令结构体是 `lcd_qspi_init_cmd_t`，实际协议是 QSPI）。

  GPIO 分配存在两份不同的参考资料，需在实施阶段核对本比赛板实际原理图确定取用哪一份：

  - 单屏通用示例（`bk_avdk_smp/projects/spi_lcd_example/ap/ap_main.c`）：
    `spi_id = 0`、`dc_pin = GPIO_28`、`reset_pin = GPIO_45`、背光 `GPIO_7`、
    LCD 外部 LDO 控制 `GPIO_13`。
  - 双屏业务方案（`BTdocs/DualScreenAVIPlayer.md`，源自 `bk_solution_ai` 双屏 AVI 播放模块，
    更贴近本项目"双 SPI LCD 屏幕"的实际场景）：
    `lcd0: spi_id=0, dc_pin=GPIO_7, reset_pin=GPIO_6`；
    `lcd1: spi_id=1, dc_pin=GPIO_5, reset_pin=GPIO_45`。

  两份资料的 GPIO 分配不一致，说明 GPIO 分配是按具体项目/PCB 走线定制的，不能直接套用任一
  参考工程的默认值。本阶段只验证一块屏幕，实施时优先使用双屏业务方案的 `lcd0` 配置
  （`spi_id=0, dc_pin=GPIO_7, reset_pin=GPIO_6`），因为它更贴近本项目双屏硬件形态；
  若实测点不亮，需回退核对硬件原理图或 `usr_gpio_cfg.h` 确认真实走线。

已核实的芯片资源归属（通过读取 `bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/` 及
`ap/components/bk_dvp/`、`ap/components/bk_camera/` 源码路径确认）：

- I2C、QSPI 的寄存器 HAL（`i2c_ll.h`、`qspi_ll.h`）位于 AP 侧 `middleware/soc/bk7258_ap/hal/`。
- DVP 控制器实现（`bk_dvp.c`）位于 AP 侧 `ap/components/bk_dvp/`。
- 结论：**DVP、I2C、QSPI 三个外设的寄存器访问全部在 AP（CPU1/CPU2）侧**，不经 Mailbox
  转发给 CP 代理，与博通"AP 负责多媒体应用，CP 负责 Wi-Fi/BLE 通信"的架构划分一致。
  CP 侧 `gpio_map.h` 中出现的 DVP 相关字段只是全局引脚复用表记录，CP 不参与实际控制。

参考示例工程（均为 AP-CP 双核架构，主要源码在 AP 目录）：

- `bk_avdk_smp/projects/dvp_example/`：摄像头独立验证工程，CLI 命令 `dvp open/close`。
- `bk_avdk_smp/projects/spi_lcd_example/`：LCD 独立验证工程，CLI 命令 `spi_lcd_display open/close`。

## 3. 目标范围（已确认）

本阶段目标是**底层驱动验证**，不做联调预览、不接 LVGL：

1. GC9D01 屏幕显示一个纯色块（对标 `spi_lcd_display open` 效果），只验证一块屏幕
   （双屏结构相同，跑通一块代表方案可行，另一块留待后续复用同一驱动路径）。
2. GC2145 摄像头采集一帧 YUV422 数据，通过日志确认帧序号、帧大小、帧率
   （对标 `dvp open <w> <h> yuv` 效果），不做编码（MJPEG/H.264）、不做与 LCD 的联调预览。

## 4. 采用方案：薄移植层（方案 A）

不追求编写标准 NuttX 字符设备驱动（`/dev/video0`、`/dev/lcdN`、`i2c_master_s`/
`spi_dev_s` lower-half）。而是将博通原厂"业务逻辑 + 寄存器操作"代码整体移植，仅替换其
依赖的 OS 原语为 NuttX 等价物，在板级 bring-up 或一个测试命令入口中直接调用，先验证
硬件通路是否可行。标准 NuttX 驱动留作后续阶段（不在本设计范围内）。

### 4.1 三层移植结构

**第一层：芯片寄存器 HAL（直接搬运，基本不改）**

- `i2c_ll.h`、`qspi_ll.h`（源自 `bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/`）
- DVP 寄存器操作（`bk_dvp.c` 内部对寄存器的读写部分）

这一层是纯寄存器读写宏/函数，不依赖 RTOS，理论上可以整体拷贝到
`board/beken/chips/bk7258/` 下，仅需确认头文件包含路径和类型定义（`uint32_t` 等）
在 NuttX 环境下可解析。

**第二层：OS 适配薄层（新写，本设计的核心工作量）**

把博通 RTOS 原语替换为 NuttX 等价物，只覆盖本阶段驱动路径实际用到的调用点：

| 博通原语 | NuttX 等价物 | 说明 |
|---|---|---|
| `rtos_create_thread` | NuttX `kthread_create` | 仅在需要独立采集线程时使用；DVP 帧回调阶段先评估是否可在中断/工作队列内完成，避免引入调度复杂度 |
| `rtos_init_semaphore` / `rtos_set_semaphore` / `rtos_get_semaphore` | `nxsem_init` / `nxsem_post` / `nxsem_wait` | |
| `os_malloc` / `os_free` | `kmm_malloc` / `kmm_free` | |
| `bk_gpio_enable_output` / `bk_gpio_set_output_high/low` / `bk_gpio_pull_up/down` | 直接寄存器操作或搬运博通 GPIO 驱动的对应函数体 | 不强行套 NuttX GPIO 框架，因为当前 bk7258 芯片层未接入通用 GPIO 子系统 |
| I2C/QSPI/DVP 中断注册 | 复用现有 `board/beken/chips/bk7258/bk7258_irq.c` 里已建立的 `up_enable_irq()`/`irq_attach()` 机制 | 需要在 `include/irq.h` 新增 I2C/QSPI/DVP 对应的 `BK7258_EXTIRQ_*` 和 `BK7258_IRQ_*` 宏，外部中断号需从 `bk_avdk_smp` 的 `icu_map.h` 交叉确认（现有 mailbox=63 已验证过这种方法） |
| `rtos_delay_milliseconds` | NuttX `nxsig_usleep` 或 `up_mdelay`（视是否在中断上下文而定） | |

**第三层：业务驱动层（直接搬运，只改 OS 调用点）**

- `dvp_gc2145.c`（传感器 I2C 初始化寄存器表，纯数据+I2C读写调用，改动量很小）
- `lcd_spi_gc9d01.c`（LCD QSPI 初始化命令表，纯数据+QSPI写调用，改动量很小）
- `bk_dvp.c`、`lcd_qspi_driver.c` 中的控制逻辑（`open`/`close`/帧缓冲管理，需要替换第二层
  列出的 OS 调用点）

**明确跳过的中间层**（避免范围膨胀，为后续标准化留出空间）：

- `bk_display_spi_new/open/close/flush` 高层显示控制器封装
- `frame_buffer_display_malloc` 帧缓冲内存池管理
- `media_service_init()`、完整 AVDK 服务框架
- `bk_pm_module_vote_ctrl_external_ldo` 电源管理投票机制 —— 本阶段用直接 GPIO 控制替代
  （LDO/背光直接拉高电平）

### 4.2 不做的事（本阶段明确排除）

- 不实现 JPEG/H.264 硬件编码器路径，只验证 YUV422 裸数据流
- 不编写标准 NuttX 字符设备节点
- 不接入 LVGL 或任何 GUI 框架
- 不做 camera 采集画面到 LCD 的联调预览
- 不验证第二块 LCD 屏幕
- 不涉及 CPU2/AP SMP

## 5. 代码落位（遵循比赛仓现有目录约定）

依据 `contest2026_264_VelaSightsuixingAIzhinengyanjing.xml` 的 linkfile 映射规则
（`board/beken/chips/bk7258/` → `vendor/beken/chips/bk7258/`，
`board/beken/boards/bk7258/bk7258-ap/` → `vendor/beken/boards/bk7258/bk7258-ap/`），
新增文件计划：

```text
board/beken/chips/bk7258/
├── bk7258_i2c.c              # 新增：I2C 控制器薄移植层（第二+第三层）
├── bk7258_qspi.c             # 新增：QSPI 控制器薄移植层
├── bk7258_dvp.c              # 新增：DVP 控制器薄移植层
├── include/irq.h             # 修改：新增 I2C/QSPI/DVP 外部中断号映射
└── hardware/
    ├── bk7258_i2c.h          # 新增：I2C 寄存器定义（从 i2c_ll.h 移植）
    ├── bk7258_qspi.h         # 新增：QSPI 寄存器定义（从 qspi_ll.h 移植）
    └── bk7258_dvp.h          # 新增：DVP 寄存器定义

board/beken/boards/bk7258/bk7258-ap/src/
├── bk7258_gc9d01.c           # 新增：GC9D01 面板初始化表 + 显示测试入口（移植自 lcd_spi_gc9d01.c）
├── bk7258_gc2145.c           # 新增：GC2145 传感器初始化表 + 采集测试入口（移植自 dvp_gc2145.c）
└── bk7258_appinit.c          # 修改：注册测试命令或在 board_app_initialize 中调用验证逻辑
```

具体测试触发方式（NSH 命令 vs board 启动时自动跑一次）留待实施计划阶段决定。

## 6. 验证标准

- LCD：屏幕显示纯色块，人工目视确认颜色正确、无花屏/撕裂。
- Camera：串口日志能看到连续帧序号递增、合理的帧大小（对应 GC2145 配置分辨率的
  YUV422 数据量）、稳定帧率，运行至少 30 秒无崩溃/无 Mailbox/heartbeat 异常
  （不能因为新驱动的中断或总线操作影响现有 AP bring-up 稳定性）。
- 两者独立验证即可，不要求同时运行（除非互不冲突，可选一起跑作为附加信心检查）。

## 7. 风险与待确认项（实施阶段处理，非本设计阶段阻塞项）

- 比赛仓 `BTdocs/` 目录下存有一批从博通文档站导出的模块说明（`VideoEngine.md`、
  `DualScreenAVIPlayer.md`、`AppEvent.md`、`FactoryConfig.md` 等），是 `bk_solution_ai`
  业务组件层文档，比通用示例工程更贴近实际产品形态，实施阶段应优先交叉核对这批文档。
- GPIO 引脚分配存在多份不一致的参考资料（见第 2 节），需要在实施时核对本比赛板实际
  原理图/`usr_gpio_cfg.h` 最终确认，不能假设任一参考工程的默认值就是本项目真实走线。
- `VideoEngine.md` 显示 DVP camera 完整调用链还包含 `frame_queue`、`network_transfer` 等
  与网络传输耦合的组件，本设计已经决定跳过这些中间层（见 4.1 节"明确跳过的中间层"），
  只搬运到 `bk_camera_ctlr`/`bk_dvp` 这一层，实施时注意不要连带引入网络传输依赖。
- I2C/QSPI/DVP 外部中断号需要从 `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/icu_map.h`
  交叉确认，方法论与现有 mailbox=63→IRQ 79 的映射一致。
- DVP 数据搬运（PCLK 时钟域数据到内存）通常依赖 DMA，需确认现有 AP bring-up 是否已具备
  可用的 DMA 驱动，若没有则需要在本阶段范围内补充最小 DMA 支持（这会增加工作量，需要在
  实施计划中显式列出）。
- MPU 内存区域划分（PORTING_PLAN 第 6.6 节）目前只覆盖 Flash/AP SRAM/PWR_MNG/外设/PSRAM/
  QSPI0/QSPI1，需确认 I2C、DVP 寄存器地址落在现有"peripheral 0x40000000-0x5fffffff"
  region 内，理论上应该覆盖，但需要实测验证无 MemFault。
