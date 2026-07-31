# 工作目录说明

`contest` 是 BK7258 OpenVela 移植开发仓库。正式代码维护在：

```text
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
├── chips/bk7258/
└── boards/bk7258/bk7258-ap/
```

本文以 `bk_avdk_smp` 为第一参考，使用其 BK7258 AP SDIO Host、SD card、寄存器和最终 `app_ab` 构建结果建立兼容基线。`bk_solution_ai` 只用于核对当前板型的 GPIO14 至 GPIO19、SD-NAND 应用方式和 FAT32 使用方式。`bk_idk` 用于交叉核对寄存器和 GPIO 复用。`openvela` 用于 NuttX `sdio_dev_s`、MMCSD 块设备和 FAT 接入。`vendor_beken` 的 BK7236N 实现只用于有限的 IRQ 和构建组织参考。

构建和提交继续遵守：

- `BK7258_OPENVELA_AP_PORTING_PLAN.md`
- `固件构建步骤.md`
- `github开发指南.md`

# BK7258 OpenVela AP SDIO 与 1 GB SD-NAND 移植设计方案 V2

> 文档状态：结合 8-pin SD-NAND 封装信息、`bk_avdk_smp/projects/app_ab` CP/AP 最终配置和 ELF、BK7258 SDIO V2 PIO 路径、NuttX MMCSD 接口及 BK7236N vendor 目录重新核对后的设计基线。本文是实施方案，不表示驱动已实现或通过实板验收。

## 1. 输入条件和固定结论

### 1.1 已知硬件条件

当前板载介质：

- 容量：1 GB。
- 封装：8 pin，焊接固定介质，不可热插拔。
- 接口：标准 SD Memory 信号，器件内部完成 NAND FTL、ECC、坏块管理和磨损均衡。
- 信号已上拉，OpenVela 不依赖内部 pull-up。
- 封装 pin 1 至 pin 8 的顺序由用户确认为：

```text
pin 1  SDD2 / DAT2
pin 2  CD/SDD3 / DAT3
pin 3  SCLK
pin 4  VSS
pin 5  CMD
pin 6  SDD0 / DAT0
pin 7  SDD1 / DAT1
pin 8  VCC
```

这里按器件封装 pin table 解释，不套用 microSD 卡座触点编号。标准 microSD 的 pin 3 是 CMD、pin 5 是 CLK，与本器件提供的封装顺序不同；PCB、器件 top/bottom view 和 pin-1 标记仍应在首次上电前复核，避免把卡座编号误用于焊接封装。

`CD/SDD3` 是 SD 协议中的 DAT3/上电 card-detect 复用信号，不是独立机械插卡检测脚。该器件固定焊接，因此 NuttX 不启用 card-detect GPIO。

### 1.2 器件引脚到 BK7258 的连接

`bk_solution_ai` 板型使用 BK7258 GPIO14 至 GPIO19：

| SD-NAND pin | SD 信号 | BK7258 GPIO | BK GPIO function | 首版 |
| ---: | --- | ---: | --- | --- |
| 1 | DAT2 | GPIO18 | `GPIO_DEV_SDIO_HOST_DATA2` | 保持 SDIO/上拉，1-bit 不传数据 |
| 2 | DAT3 | GPIO19 | `GPIO_DEV_SDIO_HOST_DATA3` | 保持 SDIO/上拉，1-bit 不传数据 |
| 3 | CLK | GPIO14 | `GPIO_DEV_SDIO_HOST_CLK` | 使用 |
| 4 | VSS | GND | - | 使用 |
| 5 | CMD | GPIO15 | `GPIO_DEV_SDIO_HOST_CMD` | 使用 |
| 6 | DAT0 | GPIO16 | `GPIO_DEV_SDIO_HOST_DATA0` | 使用 |
| 7 | DAT1 | GPIO17 | `GPIO_DEV_SDIO_HOST_DATA1` | 保持 SDIO/上拉，1-bit 不传数据 |
| 8 | VCC | 电源 rail，电压和控制方式待原理图确认 | - | 使用 |

依据：

- `bk_solution_ai/projects/beken_genie/ap/config/bk7258_ap/usr_gpio_cfg.h:36-41`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/gpio_map.h:42-47`

首版虽然只使用 DAT0 传输，仍不得把 DAT1、DAT2、DAT3 复用给 PWM、LCD 或其他外设。SD 协议上电和切换 4-bit 时需要这些线保持合法电平；当前硬件已外部上拉，软件将其配置为 SDIO function/input，不额外开启内部 pull-up，避免改变电气参数。

### 1.3 软件约束

- 当前 OpenVela AP 已完成单核启动、日志、GPIO 和 PWM。
- 当前 OpenVela AP 没有 DMA 外设适配。
- 本方案固定为 PIO-only，不新增、包装或启用 Armino GDMA。
- `CONFIG_DMA`、`CONFIG_SDIO_DMA` 和 BK7258 SDIO DMA capability 保持关闭。
- 4-bit 与 DMA 在硬件上相互独立，但本方案只在 1-bit PIO 稳定后评估 4-bit PIO。
- CPU1/OpenVela AP 是 SDIO Host 和 SD-NAND 数据路径唯一所有者。
- CPU0/CP 继续运行原厂固件，只提供既有 PWC/PM 中央时钟门控服务。
- CPU2 不访问 SDIO。

### 1.4 固定软件架构

```text
OpenVela application / NSH
          |
          v
NuttX FAT filesystem，vfat
          |
          v
/dev/mmcsd0 或 /dev/mmcsd0p0
          |
          v
NuttX drivers/mmcsd/mmcsd_sdio.c
          |
          | struct sdio_dev_s
          v
BK7258 SDIO lower-half，CPU1/AP，IRQ-assisted PIO
          |
          | GPIO14 CLK, GPIO15 CMD, GPIO16..19 DAT0..3
          v
板载 1 GB 8-pin SD-NAND

CPU0/原厂 CP
          |
          `-- PWC/PM clock gate only，不读写 sector
```

### 1.5 首版参数

| 项目 | 首版设置 |
| --- | --- |
| 介质 | 固定 present，无 hotplug/write-protect |
| 协议 | SD Memory，关闭 MMC/eMMC support |
| 总线 | 1-bit，CLK/CMD/DAT0 |
| ID clock | BK 编码 7，约 101.5625 kHz |
| transfer clock | 先 6.5 或 13 MHz，稳定后验证 20 MHz |
| 数据路径 | RX/TX FIFO 水位 IRQ 驱动 PIO |
| sector | 512 bytes |
| multi-block | 首版限制为 1 block/request |
| DMA | 明确关闭，不在本方案范围 |
| 文件系统 | 先只读识别现有 FAT32 |
| 设备节点 | `/dev/mmcsd0` |
| 自动格式化 | 禁止 |

AVDK 已在 1-bit PIO 下使用 20 MHz，但 NuttX lower-half 的 IRQ 水位搬运模型与 AVDK 线程轮询模型不同。为避免首版 ISR latency 造成 FIFO overflow/underflow，先以 6.5 或 13 MHz 验证，之后再升到 20 MHz。

### 1.6 不采用的方案

- 不使用 MTD、raw NAND、page/OOB、软件 ECC 或软件 FTL。
- 不把 Armino `sd_card_driver.c` 直接注册为 NuttX 块设备。
- 不链接 BK7236N 或 BK7258 Armino `libdriver.a` 的 SDIO 实现。
- 不修改原厂 CP 固件来代理 sector/file IO。
- 不让 CP、AP、USB MSC 同时访问同一介质。
- 不启用 DMA，不增加 DMA stub，不上报 `SDIO_CAPS_DMASUPPORTED`。
- 不照搬 AVDK 在线程中轮询 512-byte FIFO 的 RTOS semaphore 模型。
- 不首轮启用 multiblock、4-bit、自动断电、热插拔或自动格式化。

## 2. 原厂 AP/CP 所有权结论

### 2.1 `app_ab` CPU0/CP 最终固件不访问 SD-NAND

当前最终 CP 配置：

```text
bk_avdk_smp/projects/app_ab/cp/config/bk7258/config
bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258/config/sdkconfig.h
```

其中不存在以下启用项：

```text
CONFIG_SDIO_HOST
CONFIG_SDCARD
CONFIG_FATFS
CONFIG_FATFS_SDCARD
CONFIG_VFS
CONFIG_USBD_MSC
```

生成的 CP `sdkconfig.h` 到文件末尾也没有这些宏：

- `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258/config/sdkconfig.h:250-295`

CP driver init 对 SDIO 的调用受条件编译保护：

```c
#if ((CONFIG_SDIO_HOST) || (CONFIG_SDCARD))
  bk_sdio_host_driver_init();
#endif
```

依据：

- `bk_avdk_smp/cp/middleware/driver/common/driver.c:179-185`
- `bk_avdk_smp/cp/middleware/driver/common/driver.c:512-514`

当前 CP ELF 符号核对结果：

- 有通用向量符号 `SDIO_Handler`，因为 SoC vector table 覆盖所有外部中断。
- 没有 `bk_sdio_host_driver_init`、`bk_sdio_host_*`、`bk_sd_card_init`、`bk_sd_card_read_blocks` 或 `bk_sd_card_write_blocks` 实现符号。
- CP map 没有链接 `sdio_host_driver.c.obj`、`sd_card_driver.c.obj`、`disk_io.c.obj` 或 `fatfs_adapter.c.obj`。

因此，“CP 源码树存在 SDIO/SD card 代码”不能解释为“当前原厂 CP 固件访问 SD-NAND”。这些是 dormant source，只有显式打开 CP 的 `CONFIG_SDIO_HOST`/`CONFIG_SDCARD` 后才可能参与构建。

### 2.2 原厂 AP 是唯一数据面所有者

原厂 AP 最终配置明确启用：

```text
CONFIG_VFS=y
CONFIG_FATFS=y
CONFIG_FATFS_SDCARD=y
CONFIG_SDIO_HOST=y
CONFIG_SDCARD=y
CONFIG_SDIO_V2P0=y
```

依据：

- `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258_ap/config/sdkconfig.h:121-122`
- `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258_ap/config/sdkconfig.h:165-166`
- `bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/bk7258_ap/config/sdkconfig.h:243-250`

原厂 AP map/ELF 明确包含：

- `sdio_host_driver.c.obj` 和 `sdio_host_isr`。
- `sd_card_driver.c.obj`。
- `disk_io.c.obj`、FatFS 和 `fatfs_adapter.c.obj`。
- `bk_sdio_host_driver_init` 和 SDIO Handler。

构建选择：

- `bk_avdk_smp/ap/middleware/driver/CMakeLists.txt:329-335`

结论：原厂架构是 AP 直接读写 SD-NAND。OpenVela 替换原厂 AP 后，应继续由 CPU1/AP 直接拥有 Host，不引入 CP proxy。

### 2.3 CP 仍参与中央时钟管理

AP 对 `CLK_PWR_ID_SDIO` 的开关请求通过 PM Mailbox 交给 CP；CP 执行中央 clock gate 寄存器写。这是“控制面服务”，不是“存储数据访问”。

依据：

- `bk_avdk_smp/ap/middleware/driver/sys_ctrl/sys_clock_driver.c:22-40`
- `bk_avdk_smp/ap/components/bk_pm/pm.c:1389-1400`
- `bk_avdk_smp/cp/middleware/driver/pwr_clk/low_pwr_core.c:228-259`

OpenVela 必须复用既有 PWC wire ABI 请求 SDIO clock。不能因为 CP 不读写 SD-NAND，就绕过 CP 直接改中央门控。

### 2.4 CP dormant code 的防回归要求

后续每次替换 CP 固件或修改其 config，都必须检查：

```text
CONFIG_SDIO_HOST is not set
CONFIG_SDCARD is not set
CONFIG_FATFS_SDCARD is not set
CONFIG_USBD_MSC is not set
```

并检查 CP ELF：

```bash
arm-none-eabi-nm app.elf | grep -E \
  'bk_sdio_host_|bk_sd_card_|sdio_host_isr|fatfs_adapter'
```

允许只出现通用 `SDIO_Handler` vector；若出现 Host/card 实现符号，必须停止烧录并重新核定所有权。

原厂 SD card driver 在 `CONFIG_USBD_MSC && CONFIG_USB_DEVICE` 下有本地 owner vote：

- `bk_avdk_smp/cp/middleware/driver/sd_card/sd_card_driver.c:1839-1870`

当前 CP 配置未启用该路径，而且该 owner 变量只在单个固件地址空间内生效，不是 AP/CP 跨核锁。它不能防止两个核同时访问硬件，也不能作为 OpenVela 所有权协议。

## 3. BK7236N vendor 参考结论

### 3.1 有 SDIO 相关素材，但没有 NuttX SD-NAND 适配

`vendor_beken/chips/bk7236n` 包含：

- NuttX IRQ 定义：`BK7236N_IRQ_SDIO = BK7236N_IRQ_FIRST + 10`。
- Armino public headers：`sdio_host.h`、`sd_card.h` 和类型定义。
- Armino secure SDIO base：`0x458d0000 + SOC_ADDR_OFFSET`。
- `libdriver.a` 中的 GPIO SDIO mux helper 和通用 `SDIO_Handler`。

依据：

- `vendor_beken/chips/bk7236n/include/irq.h:65-76`
- `vendor_beken/chips/bk7236n/bk_idk/armino_as_lib/include/driver/sdio_host.h`
- `vendor_beken/chips/bk7236n/bk_idk/armino_as_lib/include/driver/sd_card.h`
- `vendor_beken/chips/bk7236n/bk_idk/armino_as_lib/include/soc/bk7236n/reg_base.h:90`

但没有发现：

```text
struct sdio_dev_s lower-half
mmcsd_slotinitialize()
BK7236N SDIO Kconfig option
SDIO source in chip CMakeLists/Make.defs
board MMCSD bring-up
CONFIG_MMCSD/CONFIG_FS_FAT in BK7236N nsh defconfig
```

证据：

- `vendor_beken/chips/bk7236n/CMakeLists.txt:76-146` 的 source list 无 SDIO。
- `vendor_beken/chips/bk7236n/Make.defs:45-108` 无 SDIO source。
- `vendor_beken/boards/bk7236n/bk7236n-evb/src/beken_ap.c:128-220` 无 MMCSD bind。
- `vendor_beken/boards/bk7236n/bk7236n-evb/configs/nsh/defconfig:8-124` 无 MMCSD/FAT。

`libdriver.a` 有 GPIO mux 和 vector，不等于有完整 NuttX SD block driver；BK7236N 的 generated `sdkconfig.h` 也只保留 clock 数值和 `CONFIG_SDIO_V2P0`，没有启用 `CONFIG_SDIO_HOST`、`CONFIG_SDCARD` 或 FATFS。

### 3.2 可参考和不可复用边界

可参考：

- IRQ10 的编号一致性。
- custom chip CMake/Make 条件加入 source 的组织方式。
- strong ISR registration ABI 覆盖预编译弱符号的思路。
- Armino headers 用于对照 API 语义和 clock 编码名称。

不可复用：

- BK7236N 的预编译 `libdriver.a`。
- BK7236N startup、GPIO、clock、power 或寄存器实现。
- Armino RTOS semaphore/queue/FATFS glue。
- 把 `bk_sd_card_*` 当作 NuttX `sdio_dev_s`。

因此 V2 方案以 `bk_avdk_smp` BK7258 源码为硬件主参考，以 NuttX 现有 ARM SDIO lower-half 为接口主参考，BK7236N 仅为佐证。

## 4. BK7258 Host 硬件基线

### 4.1 寄存器和安全地址

```text
SDIO secure base = 0x458d0000
TX FIFO          = base + 0x3c
RX FIFO          = base + 0x40
FIFO threshold   = base + 0x44
```

依据：

- `bk_avdk_smp/ap/include/soc/bk7258/reg_base.h:123-126`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sdio_reg.h:324-352`

当前 OpenVela AP 按 Secure 模式运行，固定使用 secure alias。寄存器生成头存在旧 base 注释、CRC shift 和 reset polarity 矛盾；实现必须把已实测行为封装在少量 helper 中，不散布 magic 写法。

### 4.2 中断

```text
INT_SRC_SDIO       = 10
CPU1 route bit     = 10
NuttX external IRQ = 10
NuttX IRQ number   = 16 + 10 = 26
```

依据：

- `bk_avdk_smp/ap/include/driver/int_types.h:98`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/icu_map.h:40`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sys_struct.h:523`

目标使用 `irq_attach(BK7258_IRQ_SDIO, ...)`、`up_enable_irq()` 和现有 CPU1 route 实现，不引入 Armino interrupt registration。

### 4.3 FIFO 和状态位

FIFO MMIO 端口为 32 bit。`SD_FIFO_THRESHOLD` 关键字段：

| bit | 含义 |
| ---: | --- |
| 0..7 | RX threshold |
| 8..15 | TX threshold |
| 18 | RX FIFO read ready，FIFO 非空 |
| 19 | TX FIFO write ready，FIFO 未满 |

依据：

- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sdio_struct.h:143-170`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/sdio_ll.h:296-314`

Host interrupt status：

| bit | 条件 |
| ---: | --- |
| 0 | command no-response complete |
| 1 | command response complete |
| 2 | command response timeout |
| 3 | receive data end |
| 4 | write data end |
| 5 | data timeout |
| 6 | RX FIFO need read |
| 7 | TX FIFO need write |
| 8 | RX FIFO overflow |
| 9 | TX FIFO empty |
| 10 | command response CRC OK |
| 11 | command response CRC fail |
| 12 | data CRC OK |
| 13 | data CRC fail |
| 14..19 | response command index |
| 20..22 | card write response status，2 表示 accepted |
| 23 | data busy |

依据：

- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sdio_struct.h:91-141`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/sdio_ll.h:399-403`

terminal status 多为 W1C。driver 只能用明确构造的 mask 清除已处理位，不能对整个 status 寄存器做读改写。`DATA_CRC_OK` 应使用 `BIT(12)`，不使用 generated header 中错误的 shift 13。

### 4.4 AVDK PIO 模型及其边界

AVDK 当前 `CONFIG_SDIO_GDMA_EN=n`，但它的“PIO”不是完整的 FIFO watermark IRQ 模式：

- command 和每块 data end 通过 IRQ/RTOS semaphore 通知。
- calling thread 轮询 `rx_fifo_rd_ready`/`tx_fifo_wr_ready` 并搬运 32-bit word。
- 默认 interrupt mask `0xe03f` 没有打开 bit6 RX need-read 和 bit7 TX need-write。

依据：

- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/sdio_ll.h:445-449`
- `bk_avdk_smp/ap/middleware/driver/sdio_host/sdio_host_driver.c:946-1027`
- `bk_avdk_smp/ap/middleware/driver/sdio_host/sdio_host_driver.c:1178-1245`
- `bk_avdk_smp/ap/middleware/driver/sdio_host/sdio_host_driver.c:1275-1443`

该模型适合 Armino 同步 API，但不直接符合 NuttX `recvsetup()/sendsetup()` 后由 `eventwait()` 等待异步完成的接口。OpenVela lower-half 应改为 IRQ-assisted PIO：ISR 在 FIFO watermark 时搬运数据，task 只等待完成事件。

### 4.5 时钟

AVDK clock enum 是寄存器编码：

| 模式 | 编码 | 参考频率 |
| --- | ---: | ---: |
| ID | 7 | 26 MHz / 256，约 101.5625 kHz |
| conservative transfer | `SDIO_HOST_CLK_6_5M=1` 或 `SDIO_HOST_CLK_13M=0` | 6.5 或 13 MHz |
| reference transfer | 14 | 320 MHz / 16，20 MHz |

依据：

- `bk_avdk_smp/ap/include/driver/hal/hal_sdio_host_types.h:25-43`
- `bk_avdk_smp/ap/middleware/driver/sdcard/sdio_driver.h:202-215`
- `bk_avdk_smp/ap/middleware/driver/sd_card/sd_card_driver.c:983-987`

编码写入前必须再次按 enum 和实测波形核对，不根据通用 SYS divider 注释猜值。首版 ID clock 和 transfer clock 均用示波器/逻辑分析仪确认。

### 4.6 PWC 时钟前置条件

OpenVela 当前 PWC transport 尚未完整消费 transport ACK 和 PM 语义响应。SDIO 开钟不能沿用 PWM 的固定延时规避方式。

必须先提供：

```text
bk7258_pwc_clock_request(CLK_PWR_ID_SDIO, ON, timeout)
  -> 发原厂兼容 PWC command
  -> 等待 ACK/completion，或轮询可靠 gate/readback
  -> 成功后才访问 0x458d0000
```

若 CP ABI 没有单独语义响应，则以有限超时的 central gate readback 或 SDIO 可访问性验证闭环。任何失败停止 MMCSD bind，返回错误，不用 blind delay。

## 5. NuttX lower-half 设计

### 5.1 绑定方式

```c
struct sdio_dev_s *dev = bk7258_sdio_initialize(0);
ret = mmcsd_slotinitialize(0, dev);
```

成功后注册 `/dev/mmcsd0`。

依据：

- `openvela/nuttx/include/nuttx/sdio.h:925-1045`
- `openvela/nuttx/include/nuttx/mmcsd.h:151-165`
- `openvela/nuttx/drivers/mmcsd/mmcsd_sdio.c:4548-4648`

NuttX `mmcsd_sdio.c` 负责 CMD0、CMD8、ACMD41、CID、CSD、RCA、SCR、选卡、ACMD6、sector addressing 和块设备。BK lower-half 不重复卡协议状态机。

### 5.2 PIO-only operation table

必须实现：

| operation | 职责 |
| --- | --- |
| `reset` | mask IRQ、停 data path、清 W1C、复位 FIFO/command/data 和软件状态 |
| `capabilities` | 首版 `SDIO_CAPS_1BIT_ONLY`；不返回任何 DMA flag |
| `status` | 固定 `SDIO_STATUS_PRESENT` |
| `widebus` | 无 transfer 时切 Host 1/4-bit |
| `clock` | disabled、ID、1-bit transfer、4-bit transfer |
| `attach` | attach IRQ26，清 pending，启用 CPU1 route/NVIC |
| `sendcmd` | 解析 decorated cmd 并配置 index/arg/response/CRC/data 方向 |
| `blocksetup` | 保存 block size、block count 和总字节数 |
| `recvsetup` | 保存 byte buffer/remaining，配置 RX PIO 和 IRQ |
| `sendsetup` | 保存 byte buffer/remaining，prime TX FIFO 并配置 IRQ |
| `cancel` | 终止 transfer、清状态并唤醒 waiter |
| `waitresponse` | 有界等待 command terminal status |
| response readers | R1/R1b/R2/R3/R6/R7；R4/R5 非空 stub 返回 `-ENOSYS` |
| `waitenable` | 设置 event mask 和 watchdog，不破坏已配置 transfer |
| `eventwait` | race-safe 等待并返回非零 event |
| media callbacks | 固定介质保存 callback，正常不触发 |

不编译 `dmarecvsetup`、`dmasendsetup` 或 `dmapreflight`，不提供伪 DMA 实现。

### 5.3 私有状态

建议：

```c
struct bk7258_sdio_s
{
  struct sdio_dev_s dev;
  sem_t waitsem;
  struct wdog_s waitwdog;
  spinlock_t lock;
  volatile sdio_eventset_t waitevents;
  volatile sdio_eventset_t wakeevent;
  uint8_t *buffer;
  size_t remaining;
  size_t blocklen;
  size_t nblocks;
  bool receive;
  bool transfer_active;
  bool command_active;
  uint32_t last_status;
  uint32_t last_cmd;
  int last_error;
  sdio_event_callback_t callback;
  void *cbarg;
};
```

实际实现不必机械照抄字段，但必须有唯一 transfer owner、authoritative `remaining`、wait race 保护和诊断快照。

### 5.4 command path

`sendcmd()` 接收的是 decorated command：

```text
MMCSD_CMDIDX_MASK
MMCSD_RESPONSE_MASK
MMCSD_DATAXFR_MASK
MMCSD_MULTIBLOCK
MMCSD_STOPXFR
MMCSD_OPENDRAIN
```

lower-half 解析后配置 hardware。响应策略：

- no response/CMD0：等待 command-no-response complete。
- R1/R1b/R6/R7：校验 timeout、CRC 和 response command index。
- R2/CID/CSD：读取 4 words，按 NuttX 预期顺序返回。
- R3/OCR：不要求 CRC。
- hardware response timeout 返回 `-ETIMEDOUT`。
- CRC/index error 返回 `-EIO`，不能退化成软件 queue timeout。

### 5.5 RX IRQ-assisted PIO

`recvsetup()` 必须支持至少：

- 8-byte SCR。
- 512-byte sector。
- 后续多块的 `n * 512`。

流程：

1. 验证无 active transfer，保存 byte buffer 和总长度。
2. 配置 block size、single/multi、RX direction 和 hardware data timer。
3. 清 stale RX end/timeout/CRC/overflow/need-read status。
4. 配置经实测的 RX threshold。
5. enable RX need-read、RX end、data timeout、CRC fail 和 overflow IRQ。
6. card command 启动后，ISR 在 RX watermark 或 RX end 时搬运。

ISR 算法：

```text
read enabled pending status
if timeout/CRC fail/overflow:
    recover + wake ERROR/TIMEOUT
if RX_NEED_READ or RX_END:
    while remaining > 0 and RX_READY:
        word = RX_FIFO
        copy min(remaining, 4) bytes
if RX_END:
    final drain while RX_READY
    success only when remaining == 0 and data CRC OK
clear processed W1C bits
```

每次 ISR 搬运必须设置固定 word budget。达到 budget 后若 FIFO 仍可读，则保留 watermark IRQ 使其再次触发，不能在一次 ISR 中无界清空未知深度的 FIFO。budget 从 16 或 32 words 起步，以 FIFO threshold、IRQ 重触发行为和最坏中断延迟实测确定。

不能在每个硬件 block-end 后假定整个请求完成；software `remaining` 归零且 terminal status 合法才完成。

### 5.6 TX IRQ-assisted PIO

NuttX 普通 PIO write 顺序是 CMD24/R1 后 `blocksetup -> waitenable -> sendsetup -> eventwait`。首版单块写：

1. 保存 source byte buffer 和 512-byte length。
2. 清 stale TX end/timeout/CRC/FIFO status。
3. 配置 TX threshold、data timer 和 single block。
4. 使用 `TX_READY` 先尽可能 prime FIFO。
5. 启动 write state machine。
6. enable TX need-write、write end、timeout 和 error IRQ。
7. ISR 在 TX need-write 时继续填 FIFO。
8. `remaining == 0` 后关闭 TX watermark IRQ，但保留 write-end/error IRQ。
9. 只有 `DATA_WR_END` 且 card write-response status 为 2 才成功。

TX ISR 同样使用固定 word budget。达到 budget 时只要还有数据，就保持 TX watermark IRQ；不能为了填满未知深度 FIFO 长时间占用 IRQ26。

`TX_FIFO_EMPTY` 只表示 Host FIFO 空，不表示 card 已经接受或完成 program，不能作为 transfer done。

### 5.7 alignment 和 byte order

禁止：

```c
*(uint32_t *)buffer
```

AVDK 当前 TX 路径存在任意用户 buffer 未对齐访问风险。OpenVela 保留 `uint8_t *`，显式 little-endian 组装/拆分：

```c
word = p[0] |
       ((uint32_t)p[1] << 8) |
       ((uint32_t)p[2] << 16) |
       ((uint32_t)p[3] << 24);
```

末尾不足 4 bytes 时 word 先清零，只复制有效字节。RX 也只写 `min(remaining, 4)`。这样支持任意 alignment、8-byte SCR 和未来非 4 倍数 transfer。

FIFO byte-select 寄存器沿用经验证的 AVDK RX 设置，但以 sector pattern 和 FAT signature `55 aa` 验证，不因协议字段显示顺序额外猜测 software byteswap。R2 response word order 与 data FIFO byte order 分别测试。

### 5.8 wait/event 竞态

NuttX 可能出现：

```text
waitenable
recvsetup/sendsetup
sendcmd 或 start transfer
IRQ 在此完成
eventwait
```

也有 SCR path 先 `recvsetup` 再 `waitenable`。因此：

- `recvsetup` 配置好的 RX path 不能被随后的 `waitenable` reset。
- `waitenable` 清 stale wait event 和 terminal status，但不破坏当前 transfer config。
- ISR 先保存 `wakeevent`，disable 相关 IRQ，cancel watchdog，再 post semaphore。
- `eventwait` 必须能消费已经 post 的 event，不得再次睡眠。
- `eventwait` 每条路径都 cancel watchdog、关闭 wait mask 并返回非零 event。

参考结构：

- `openvela/nuttx/arch/arm/src/lpc17xx_40xx/lpc17_40_sdcard.c:1066-1080`
- `openvela/nuttx/arch/arm/src/lpc17xx_40xx/lpc17_40_sdcard.c:2195-2347`

### 5.9 timeout

两级超时：

1. BK hardware command/data timer，提供精确 terminal IRQ。
2. NuttX watchdog，防止 IRQ 丢失、clock 停止或 controller 锁死。

规则：

- hardware timeout ISR 立即停止 transfer 并 wake waiter，不能只清 bit 后继续等 watchdog。
- hardware timer 略短于 NuttX timeout。
- 不使用 CPU loop count 模拟时间。
- command timeout 区分 `-ETIMEDOUT` 和 CRC `-EIO`。
- 首版沿用 AVDK timer count 作为起点，但用 SDCLK 实测校准。

### 5.10 cancel 和恢复

CRC fail、timeout、RX overflow、TX underflow、terminal status 不一致或 watchdog 触发时：

1. mask FIFO watermark 和 data terminal IRQ。
2. 停止 data engine/SDCLK 自动发送。
3. 保存 `last_status`、`remaining` 和 command。
4. 用明确 W1C mask 清已知 status。
5. 用单一、实测过的 helper 复位对应 FIFO 和 SD state。
6. 清 software transfer state 和 stale semaphore。
7. wake waiter 为 ERROR 或 TIMEOUT。
8. 若后续是 multiblock，安全状态下发送 CMD12。
9. 下一 command 仍失败则 reset controller 并重新枚举。
10. power-cycle SD-NAND 只作为最后恢复手段。

AVDK register comments 对 FIFO/state reset polarity 互相矛盾。实施前通过 golden register trace 或受控 FIFO 测试确定序列，并只保留一个 `bk7258_sdio_reset_datapath()`。

ISR 禁止动态分配、FAT 操作、PWC request、busy wait card program 或逐 watermark 日志。

IRQ26 的单次执行时间必须纳入现有 Mailbox/PWC heartbeat、日志和 PWM 中断延迟测试。若 watermark IRQ 是电平触发，ISR 返回前须确认已处理状态不会立即形成无进展的中断风暴；若达到 budget 后仍有进展空间，则允许下一次 IRQ 继续搬运。

## 6. 4-bit PIO 策略

8-pin 器件完整引出 DAT0 至 DAT3 且均上拉，电气条件支持 4-bit。4-bit 与 DMA 无依赖：AVDK bus width 和 DMA 是独立配置，Host data width 也是独立 bit。

但本方案不从首版直接启用 4-bit，原因是相同 clock 下 byte rate 提高 4 倍，PIO ISR latency 要求更高。

启用顺序：

1. 1-bit，6.5/13 MHz，single block 稳定。
2. 1-bit，20 MHz 稳定。
3. 打开 RX/TX watermark IRQ 压力测试，确认无 overflow/underflow。
4. capabilities 从 `SDIO_CAPS_1BIT_ONLY` 改为 `SDIO_CAPS_4BIT`。
5. NuttX 读取 SCR 并发 ACMD6，随后调用 `widebus(true)`。
6. 4-bit 先降到 6.5/13 MHz，不同时升频。
7. 4-bit 20 MHz 只在 IRQ latency 和长稳通过后启用。

`widebus()` 只改 Host 侧 data width，card 侧 ACMD6 由 MMCSD 上层负责。不要复制 AVDK 中 `CONFIG_SDIO_4LINES_EN` 和 `CONFIG_SDCARD_BUSWIDTH_4LINE` 两套 symbol 分裂，OpenVela 使用一个 runtime width state。

如果 4-bit PIO 在系统高负载下出现 FIFO 错误，正式方案退回 1-bit 20 MHz，不以引入未适配 DMA 来掩盖问题。

## 7. 文件系统和介质布局

### 7.1 SD-NAND 作为块设备

软件只看到标准 SD sector。1 GB 不硬编码 sector count，使用 CSD 和 `BIOC_GEOMETRY`：

```text
sector size = 512
capacity = geo_nsectors * 512
```

商品 1 GB 可能约 1,953,125 sectors，也可能接近 2,097,152 sectors。验收以实测 geometry、CSD 和原厂 golden 一致为准。

### 7.2 首次只读识别

先读取 sector 0 和 LBA1：

| 布局 | 识别 | 设备路径 |
| --- | --- | --- |
| FAT superfloppy | sector 0 直接是 FAT BPB | `/dev/mmcsd0` |
| MBR + FAT | MBR entry 和 `0x55aa` | 注册 `/dev/mmcsd0p0` |
| GPT + FAT | protective MBR，LBA1 `EFI PART` | 注册 GPT partition |
| unknown | 无合法结构 | 停止，不写入 |

NuttX MBR partition 不会自动注册命名设备。需要时使用 `parse_block_partition()` 和自定义 handler：

- `openvela/nuttx/include/nuttx/fs/partition.h:49-83`
- `openvela/nuttx/boards/risc-v/mpfs/common/src/mpfs_emmcsd.c:47-79`

### 7.3 FAT

使用 NuttX 原生：

```text
CONFIG_FS_FAT=y
mount type = vfat
mount point = /mnt/sdnand
```

不使用 Armino 的 `CONFIG_FATFS`、`bk_fatfs_partition` 或 `FATFS_DEV_SDCARD`。

首轮禁止：

- `mkfatfs`。
- `dd` 写整盘。
- destructive block test。
- 自动 fsck 修复或自动格式化。

只有完成备份、确认设备路径、unmount 且产品明确允许后，才单独启用 `CONFIG_FSUTILS_MKFATFS`。

### 7.4 唯一所有权

当前原厂 CP 不访问 SD-NAND，但未来 USB device MSC 或 CP config 改变会引入冲突。正式状态机：

```text
OpenVela owns
  -> CP SDIO/SDCARD disabled
  -> USB MSC disabled
  -> OpenVela may mount

transfer to USB MSC，未来功能
  -> fsync/sync
  -> unmount
  -> stop transfer and verify card not busy
  -> only then expose block device

return to OpenVela
  -> USB eject/disconnect
  -> re-enumerate or media reset
  -> mount
```

本阶段不实现 USB MSC，不增加跨核 storage proxy 或 owner lock。

### 7.5 掉电

固定 NAND 可能有内部 write cache。`fsync()` 只保证请求送到 block layer，不证明器件内部掉电安全。

正常关机：

```text
block new writers
fsync all files
sync filesystem
unmount
wait active transfer complete
CMD13/card state 确认不 busy
满足 datasheet power-off 延时
再关闭 VCC rail/系统电源
```

在没有器件型号和 datasheet 前，不声称掉电不丢数据。必须执行 idle、写入中、fsync 后、unmount 后四类受控断电测试。

## 8. 目标代码布局

```text
board/beken/chips/bk7258/
├── Kconfig
├── CMakeLists.txt
├── Make.defs
├── bk7258_sdio.c
├── bk7258_sdio.h
├── include/irq.h
└── hardware/
    ├── bk7258_memorymap.h
    ├── bk7258_sdio.h
    └── bk7258_sysctrl.h

board/beken/boards/bk7258/bk7258-ap/
├── include/board.h
├── src/CMakeLists.txt
├── src/bk7258_mmcsd.c
├── src/bk7258_bringup.c
└── configs/nsh/defconfig
```

边界：

- chip 层：寄存器、IRQ、clock mapping、command、FIFO 和 `sdio_dev_s`。
- board 层：GPIO14 至 GPIO19、VCC rail、电源延时、MMCSD bind 和 mount。
- PWC 层：CP-compatible clock request 及完成等待。
- 不增加 DMA source、DMA section 或 DMA Kconfig。

## 9. Kconfig/defconfig

### 9.1 chip Kconfig

```kconfig
config BK7258_SDIO
	bool "BK7258 SDIO host"
	select ARCH_HAVE_SDIO
	select SDIO_BLOCKSETUP
	help
		Enable the BK7258 AP SDIO host in interrupt-assisted PIO mode.
```

不定义 `BK7258_SDIO_DMA`，防止未适配功能被误认为可用。

### 9.2 首版 defconfig

```text
CONFIG_BK7258_SDIO=y

CONFIG_MMCSD=y
CONFIG_MMCSD_SDIO=y
# CONFIG_MMCSD_SPI is not set
CONFIG_MMCSD_NSLOTS=1
CONFIG_MMCSD_MULTIBLOCK_LIMIT=1
# CONFIG_MMCSD_MMCSUPPORT is not set
# CONFIG_MMCSD_HAVE_CARDDETECT is not set
# CONFIG_MMCSD_HAVE_WRITEPROTECT is not set
# CONFIG_MMCSD_SDIOWAIT_WRCOMPLETE is not set

CONFIG_SDIO_BLOCKSETUP=y
# CONFIG_SDIO_DMA is not set
# CONFIG_DMA is not set

CONFIG_SCHED_HPWORK=y

CONFIG_FS_FAT=y
CONFIG_NSH_MMCSDMINOR=0

CONFIG_DEBUG_MEMCARD=y
CONFIG_DEBUG_MEMCARD_ERROR=y
CONFIG_DEBUG_MEMCARD_WARN=y
CONFIG_DEBUG_MEMCARD_INFO=y
```

稳定后关闭 INFO，保留 error/warn 和 driver counters，避免 Mailbox 日志改变 PIO 时序。

## 10. 分阶段实施

### P0：原厂 golden 取证

使用未修改的 `bk_avdk_smp/projects/app_ab` 原厂 AP：

1. 记录 CP/AP 最终 config 和 ELF SDIO 符号，确认只有 AP 有数据面。
2. 记录 8-pin 器件型号、pin-1 方向、VCC 电压和上电延时。
3. 记录 GPIO14 至 GPIO19 波形和 SDCLK。
4. 导出 OCR、CID、CSD、SCR、RCA 和容量。
5. dump sector 0/LBA1，记录文件系统布局。
6. 备份数据并记录关键文件 hash。

退出：硬件 pin、电源、容量、布局和 golden 响应全部固定。

### P1：PWC 和 Host 寄存器

1. 完成 SDIO clock request 的 ACK/completion 或 readback 闭环。
2. 添加 secure base 和 IRQ26。
3. 配置 GPIO14 至 GPIO19，内部 pull disabled。
4. 验证 ID/transfer clock 波形。
5. 验证 reset、W1C 和 FIFO ready 行为。

板级调用顺序固定为：

```text
bk7258_pwc_start()
  -> PWC transport/worker ready
  -> request SDIO clock and wait completion
  -> configure SD-NAND power and pinmux
  -> bk7258_sdio_initialize(0)
  -> mmcsd_slotinitialize(0, dev)
  -> optional read-only mount
```

SDIO 初始化失败应记录错误并继续提供 NSH/日志，不应让 `board_late_initialize()` 整体失效；但不得注册虚假的 `/dev/mmcsd0`。

退出：100 次冷启动均能确定开钟成功或有限超时，不随机继续。

### P2：command-only lower-half

实现 reset、clock、attach、sendcmd、waitresponse 和 responses，按序验证：

```text
CMD0
CMD8 R7 = 0x1aa
CMD55 + ACMD41 OCR ready
CMD2 CID
CMD3 RCA
CMD9 CSD
CMD7 select
ACMD51 SCR，进入下一阶段 8-byte RX
```

退出：CID/CSD/OCR 与 golden 一致；断电器件返回有限 timeout，无 hard fault。

### P3：PIO RX

1. 实现 8-byte SCR IRQ RX。
2. 实现 512-byte CMD17。
3. 验证 unaligned destination。
4. 注册 `/dev/mmcsd0`，只读 geometry 和 sector。
5. 对比 golden sector hash。

退出：连续和随机只读无 CRC/overflow，timeout/cancel 后可继续命令。

### P4：PIO TX

1. 在备份后的专用测试 LBA 实现 CMD24。
2. 验证 FIFO refill、write-end 和 response status 2。
3. pattern 写回读：zero、ones、walking bit、PRNG。
4. 验证 unaligned source。
5. 错误后 cancel/reset 恢复。

退出：至少 10,000 次随机单块读写无 silent corruption。

### P5：FAT32

1. 根据 P0 选择 whole disk 或 partition 设备。
2. 首次只读 `vfat` mount `/mnt/sdnand`。
3. 对比目录、文件长度和 hash。
4. 再执行受控 create/write/fsync/read/rename/unlink。
5. mount/unmount 和正常掉电回归。

退出：原数据无损，重新上电和挂载后一致。

### P6：multiblock PIO

1. 解除 `MMCSD_MULTIBLOCK_LIMIT=1`。
2. 验证 CMD18/CMD25/CMD12 和 software block count。
3. 测试 2/4/8/32/128 sectors。
4. 覆盖方向切换、非连续 LBA、timeout 和 CRC 恢复。

退出：多块 hash 一致、无额外块预读泄露、CMD12 后 card state 正确。

### P7：4-bit PIO，可选

按第 6 节顺序启用，不引入 DMA。若 20 MHz 4-bit 在系统高负载下无法满足 FIFO 时序，正式退回可靠的 1-bit 模式。

## 11. 验证矩阵

### 11.1 构建和所有权

- OpenVela `-Werror` CMake 构建通过。
- `all-app.bin` 打包和 AP binary hash 通过。
- CP config 不含 SDIO/SDCARD/FATFS/MSC。
- CP ELF 只允许通用 vector，不含 Host/card 实现。
- OpenVela 不链接 `libdriver.a` SDIO object。
- `.config` 明确 `DMA=n` 和 `SDIO_DMA=n`。

### 11.2 枚举

| 用例 | 预期 |
| --- | --- |
| 冷启动 100 次 | `/dev/mmcsd0` 稳定出现 |
| soft reset | CID/CSD/容量一致 |
| VCC off | 有限 timeout，无死锁 |
| clock request fail | 不访问 Host 寄存器 |
| geometry | sector 512，容量与 1 GB 器件实测一致 |
| sector0 | 与 golden 完全一致 |

### 11.3 PIO

| 用例 | 范围 |
| --- | --- |
| SCR | 8 bytes |
| single read/write | first/middle/last test LBA |
| alignment | offset 0/1/2/3/4/8/16/32 |
| random | 10,000 次起 |
| direction | read-write-read 循环 |
| error | command/data timeout、CRC、overflow、cancel |
| load | Wi-Fi、BT、日志、PWM 并发 |

driver 必须统计：

```text
cmd_count, cmd_timeout, cmd_crc_error
rx_words, tx_words, rx_blocks, tx_blocks
data_timeout, data_crc_error
rx_overflow, tx_underflow
cancel_count, reset_count
last_cmd, last_status, last_error, last_remaining
```

### 11.4 FAT 和长稳

- 只读 mount/list/hash。
- create/write/fsync/read/rename/unlink。
- 0-byte、小文件、大文件和空间耗尽。
- 1000 次 mount/unmount。
- 24 小时循环 IO 和 hash。
- 100 次冷启动。
- idle、write 中、fsync 后、unmount 后受控断电。

`cmocka_driver_block` 会破坏约 95% 介质，只能对备份后的专用测试介质/分区执行，不能对生产 FAT 卷执行。

### 11.5 性能

记录但不以性能牺牲正确性：

- 1-bit 6.5/13/20 MHz sequential throughput。
- 4 KiB random IOPS。
- IRQ rate 和单次 ISR 最大搬运量。
- CPU utilization 和最坏 ISR latency。
- 可选 4-bit PIO throughput。

无 DMA 前提下，性能验收必须包含系统并发负载，不只测空闲系统。

## 12. 完成标准

1. 8-pin pin order、GPIO14 至 GPIO19 和 VCC 连接有原理图/实测闭环。
2. 原厂 CP 最终固件不含 SDIO 数据路径，后续构建有自动符号检查。
3. CPU1/OpenVela 是唯一 Host/sector owner。
4. PWC clock request 有确定完成机制，不使用 blind delay。
5. secure base `0x458d0000`、IRQ26、W1C 和 reset 序列实测正确。
6. lower-half 是 NuttX 原生 IRQ-assisted PIO，不混入 Armino RTOS/FATFS。
7. DMA 保持关闭，不上报 DMA capability，不存在 DMA 代码依赖。
8. `/dev/mmcsd0` 稳定枚举，geometry 为 512-byte sector 和真实 1 GB 容量。
9. SCR、sector、unaligned buffer、timeout 和 cancel 测试通过。
10. 单块和多块测试无 silent corruption。
11. 现有 FAT32 无损挂载，未自动格式化。
12. 24 小时、100 次冷启动、1000 次 mount 和掉电测试达到门槛。
13. 4-bit 若启用，必须在无 DMA 和高系统负载下通过 FIFO/CRC 长稳；否则交付 1-bit。
14. USB MSC、CP dormant driver 和 CPU2 均不会并发访问介质。

## 13. 待确认事项

| 项目 | 当前状态 | 方法 |
| --- | --- | --- |
| SD-NAND 厂商/型号 | 未提供 | 丝印、BOM、datasheet |
| pin table top/bottom view | pin 顺序已知，视图待复核 | datasheet 和 PCB |
| VCC 电压/rail 控制 | 未知 | 原理图和示波器 |
| power-on/off 延时 | 未知 | datasheet |
| 精确 sector count | 未知 | CSD/geometry |
| superfloppy/MBR/GPT | 未知 | sector0/LBA1 |
| FIFO threshold 单位/深度 | 生成注释不足 | register test 和逻辑分析仪 |
| FIFO/SD reset polarity | 源码注释冲突 | golden trace 和受控测试 |
| conservative clock 编码 | 待核对 | enum 和 SDCLK 波形 |
| PWC completion ABI | OpenVela 尚未闭环 | CP handler 和抓包 |
| 4-bit PIO 最大可靠频率 | 未知 | 并发压力和 SI 测试 |
| SD-NAND 内部掉电保证 | 未知 | 器件 datasheet 和断电测试 |

## 14. 推荐提交顺序

1. `docs: refine BK7258 SDIO PIO porting plan`
2. `fix: complete AP PWC clock request handshake`
3. `feat: add BK7258 SDIO command lower-half`
4. `feat: add BK7258 SDIO IRQ PIO receive path`
5. `feat: add BK7258 SDIO IRQ PIO transmit path`
6. `feat: bind BK7258 SD-NAND to MMCSD`
7. `feat: mount BK7258 SD-NAND FAT volume`
8. `feat: enable BK7258 SDIO multiblock PIO`
9. `feat: enable BK7258 SDIO four-bit PIO`

本阶段没有 DMA 提交。4-bit 必须与 multiblock 分开提交，每个提交包含构建结果和对应实板验证记录。
