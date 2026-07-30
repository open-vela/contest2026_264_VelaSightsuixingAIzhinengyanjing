# 工作目录说明

`contest` 是 BK7258 OpenVela 移植开发仓库。所有正式移植代码必须维护在：

```text
contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
├── chips/bk7258/
└── boards/bk7258/bk7258-ap/
```

`bk_solution_ai` 是当前开发板 SD-NAND 的主要参考工程。它本身只提供项目配置、GPIO 和应用层挂载示例，实际 SDIO Host、SD Memory 和 FATFS 实现来自 `bk_avdk_smp/ap`。`bk_idk` 用于交叉核对 BK7258 寄存器、时钟、中断和 GPIO 复用。`openvela` 用于确认 NuttX MMC/SD lower-half、块设备和 FAT 文件系统的标准接入方式。

构建和提交继续遵守：

- `BK7258_OPENVELA_AP_PORTING_PLAN.md`
- `固件构建步骤.md`
- `contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/github开发指南.md`

# BK7258 OpenVela AP SDIO 与板载 SD-NAND 移植实施方案

> 文档状态：基于当前 OpenVela AP、`bk_solution_ai`、`bk_avdk_smp` 和 `bk_idk` 源码交叉验证后的实施基线。当前板载介质容量由项目需求确认为 1 GB，但工作区参考资料记录的是 128 MB 版本，介质型号、供电连接和分区形式仍须从当前实板原理图及 CID/CSD 实测确认。本文只生成实施方案，不表示 SDIO 已完成移植或实板验收。

## 1. 目标和固定结论

### 1.1 移植目标

在物理 CPU1 单核运行的 OpenVela AP 上接管 BK7258 SDIO Host，并将板载 1 GB SD-NAND 接入 NuttX 标准存储栈，完成：

- SDIO Host 时钟、GPIO、中断、命令和 PIO 数据传输。
- 标准 SD Memory 初始化和 SDSC/SDHC 地址模式识别。
- NuttX MMC/SD 块设备 `/dev/mmcsd0`。
- 现有 FAT32 数据卷识别和挂载。
- 512 字节扇区的单块、多块、同步和异常恢复。
- 固定介质的掉电、复位、超时和文件系统一致性验证。
- 后续 4-bit 和 DMA 优化所需的边界与验收条件。

保持以下内容原样：

- CPU0/CP 固件、bootloader、SPI Flash 分区和 AP 镜像打包流程。
- OpenVela AP 的 Secure 地址模型、单核 CPU1 启动和 Mailbox/PWC 契约。
- 板载 SD-NAND 内部的 FTL、ECC、坏块和磨损均衡实现。

### 1.2 固定架构

```text
OpenVela application / NSH
          |
          v
NuttX FAT filesystem (vfat)
          |
          v
/dev/mmcsd0 或 /dev/mmcsd0p0
          |
          v
NuttX mmcsd_sdio.c
          |
          | struct sdio_dev_s
          v
BK7258 SDIO lower-half，CPU1/AP
          |
          | CLK/CMD/DAT0，后续 DAT1..3
          v
板载 1 GB SD-NAND，标准 SD Memory 协议
```

本方案中：

- SD-NAND 是内部带控制器、通过 SD Memory 命令提供 512 字节 LBA 的固定块设备，不是裸 NAND。
- 不实现 NuttX MTD、NAND page/OOB、ECC 或 FTL 驱动。
- NuttX `mmcsd_sdio.c` 负责 CMD0、CMD8、CMD55/ACMD41、CID、CSD、RCA、选卡、总线宽度和块设备注册。
- BK7258 lower-half 只负责控制器、命令、响应、FIFO、IRQ、等待事件、时钟和总线宽度。
- CPU1/AP 是 SDIO 控制器和 IRQ 的唯一所有者；CPU0/CP 只按既有 PM Mailbox 协议完成中央时钟门控请求。
- 首版不由 CP 代理文件访问，也不把 Armino FATFS 或 `sd_card_driver.c` 链接进 OpenVela。

### 1.3 首版兼容基线

为降低首轮调试变量，首版固定为：

| 项目 | 首版设置 | 依据 |
| --- | --- | --- |
| 介质 | 固定、不可热拔插 | 板载 SD-NAND |
| 协议 | SD Memory | 原厂只使用标准 SD 命令，无 NAND page/OOB 操作 |
| 总线 | 1-bit，CLK/CMD/DAT0 | `bk_solution_ai` 最终配置关闭 4-line |
| 识别时钟 | 编码 7，约 101.5625 kHz | 26 MHz / 256 |
| 传输时钟 | 编码 14，20 MHz | 320 MHz / 16 |
| 数据路径 | 中断辅助 PIO | 原厂最终配置关闭 SDIO GDMA |
| 块大小 | 512 字节 | 原厂 FATFS glue 和 NuttX MMCSD |
| 多块传输 | 第一阶段限制为单块 | 先隔离 CMD/IRQ/FIFO 问题 |
| 文件系统 | 读取现有 FAT32，NuttX `vfat` | 避免首轮格式化破坏板载数据 |
| 设备节点 | `/dev/mmcsd0` | `mmcsd_slotinitialize(0, dev)` |
| 自动格式化 | 禁止 | 防止探测错误导致数据破坏 |
| DMA | 关闭 | 避免地址可达性、cache 和对齐问题 |

稳定后按“多块传输 -> 4-bit -> DMA”的顺序优化，每一步均须单独回归。

### 1.4 本轮明确不采用的方案

- 不移植 legacy `bk_avdk_smp/ap/middleware/driver/sdcard/` 实现。
- 不把新的 `sd_card_driver.c` 整体作为 NuttX 块驱动使用。
- 不在 BK lower-half 重复实现完整 SD 卡状态机。
- 不将板载介质注册为 MTD 或裸 NAND。
- 不在第一阶段启用 GDMA、D0 write-complete GPIO 模式、热插拔或自动断电。
- 不默认格式化、重建分区表或擦除现有 1 GB 介质。
- 不假设参考板的 128 MB 容量、GPIO52 供电和 FAT superfloppy 布局与当前实板完全一致。
- 不让 CPU0、CPU1 和 CPU2 并发访问同一个 SDIO 控制器。

## 2. 原厂实现和代码依据

### 2.1 `bk_solution_ai` 实际复用 AVDK 驱动

`bk_solution_ai` 是 AVDK 的 solution overlay，不包含独立的 SDIO Host 源码。其 Make/CMake 工程选择 `bk_avdk_smp` 作为 SDK，并在其上叠加项目组件：

- `bk_solution_ai/projects/volc_rtc/Makefile:1-7`
- `bk_solution_ai/projects/volc_rtc/CMakeLists.txt:5-11`
- `bk_avdk_smp/tools/build_tools/cmake/project.cmake:252-277`

实际选中的新驱动是：

```text
bk_avdk_smp/ap/middleware/driver/sdio_host/sdio_host_driver.c
bk_avdk_smp/ap/middleware/driver/sd_card/sd_card_driver.c
bk_avdk_smp/ap/middleware/soc/common/hal/sdio_host_hal.c
bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/sdio_ll.h
```

构建入口：

- `bk_avdk_smp/ap/middleware/driver/CMakeLists.txt:329-335`

以下旧目录虽然名称更直接，但不是 `bk_solution_ai` 当前配置选择的实现：

```text
bk_avdk_smp/ap/middleware/driver/sdcard/
```

特别是该旧实现存在不适用于 BK7258 的 legacy base address 分支，不应作为寄存器移植基线。

### 2.2 参考工程最终配置

`beken_genie`、`volc_rtc`、`volc_rtc_ab` 和 `ai_camera` AP 配置共同启用：

```text
CONFIG_VFS=y
CONFIG_FATFS=y
CONFIG_FATFS_SDCARD=y
CONFIG_SDIO_HOST=y
CONFIG_SDIO_V2P0=y
CONFIG_SDCARD=y
CONFIG_SDIO_HOST_DEFAULT_CLOCK_FREQ=7
CONFIG_SDCARD_DEFAULT_CLOCK_FREQ=14
CONFIG_SDIO_GDMA_EN=n
CONFIG_SDIO_4LINES_EN=n
CONFIG_SDCARD_BUSWIDTH_4LINE=n
CONFIG_SDCARD_CHECK_INSERTION_EN=n
CONFIG_SDCARD_POWER_GPIO_CTRL=y
CONFIG_SDCARD_POWER_GPIO_CTRL_AUTO_POWERDOWN_WHEN_IDLE=n
```

代表性依据：

- `bk_solution_ai/projects/beken_genie/ap/config/bk7258_ap/config:800-801`
- `bk_solution_ai/projects/beken_genie/ap/config/bk7258_ap/config:979-980`
- `bk_solution_ai/projects/beken_genie/ap/config/bk7258_ap/config:1456-1484`

该配置说明当前稳定参考是固定介质、1-bit、20 MHz、PIO，不是 4-bit DMA 高性能路径。

### 2.3 SD-NAND 的软件模型

`bk_solution_ai` 文档把参考板的 SD-NAND 描述为 128 MB、默认 FAT32、通过 VFS/FATFS 访问：

- `bk_solution_ai/docs/bk7258/zh_CN/projects/beken_genie/index.rst:123-127`
- `bk_solution_ai/docs/bk7258/zh_CN/projects/volc_rtc/index.rst:105-109`

AVDK FATFS Kconfig 把 “SDCARD/SD-NAND” 归入同一个 SDIO Host slave 类型：

- `bk_avdk_smp/ap/components/fatfs/Kconfig:7-14`

原厂 SD 驱动只发标准 SD Memory 命令，并按 OCR/CSD 区分 SDSC 和 SDHC/SDXC：

- `bk_avdk_smp/ap/middleware/driver/sd_card/sd_card_driver.h:34-53`
- `bk_avdk_smp/ap/middleware/driver/sd_card/sd_card_driver.c:835-996`

因此当前 1 GB 器件也应先按固定 SD Memory 设备探测。必须通过 CMD8、ACMD41、CID 和 CSD 实测确认，不能仅按商品名“SD NAND”推断协议或容量。

### 2.4 原厂初始化命令链

AVDK 的初始化顺序为：

```text
Host reset / 100 kHz / 1-bit
  -> CMD0  GO_IDLE_STATE
  -> 50 ms delay
  -> CMD8  SEND_IF_COND, arg 0x1aa
  -> CMD55 + ACMD41，轮询 OCR busy
  -> CMD2  ALL_SEND_CID
  -> CMD3  SEND_RELATIVE_ADDR
  -> CMD9  SEND_CSD
  -> CMD7  SELECT_CARD
  -> CMD55 + ACMD6，选择 1-bit 或 4-bit
  -> CMD16 SET_BLOCKLEN, 512
  -> 切换为 20 MHz
```

代码依据：

- `bk_avdk_smp/ap/middleware/driver/sd_card/sd_card_driver.c:218-503`
- `bk_avdk_smp/ap/middleware/driver/sd_card/sd_card_driver.c:835-996`
- `bk_avdk_smp/ap/middleware/driver/sd_card/sd_card_driver.c:1005-1123`

NuttX `drivers/mmcsd/mmcsd_sdio.c` 已实现同类卡协议。因此移植应以该命令链作为波形和响应对照，不应复制成第二套上层状态机。

### 2.5 原厂读写路径

AVDK 当前 V2 路径使用：

- SDHC/SDXC：命令参数直接使用 LBA。
- SDSC：LBA 乘以 512，转换为 byte address。
- 读：CMD18 多块读取。
- 写：CMD25 多块写入。
- 连续请求保持流打开，地址或方向变化时发送 CMD12。
- 命令完成、TX 块完成和 RX 块完成由 IRQ 通知。
- 数据由 CPU 轮询 FIFO ready 后按 32 bit 搬运，GDMA 关闭。

依据：

- `bk_avdk_smp/ap/middleware/driver/sd_card/sd_card_driver.c:1283-1449`
- `bk_avdk_smp/ap/middleware/driver/sd_card/sd_card_driver.c:1451-1655`
- `bk_avdk_smp/ap/middleware/driver/sdio_host/sdio_host_driver.c:946-1027`
- `bk_avdk_smp/ap/middleware/driver/sdio_host/sdio_host_driver.c:1199-1245`

NuttX 首版不复刻“跨请求保持 CMD18/CMD25 打开”的优化。先使用 MMCSD 上层标准单请求生命周期，配合 `CONFIG_MMCSD_MULTIBLOCK_LIMIT=1` 验证正确性；稳定后再解除多块限制。

### 2.6 原厂文件系统挂载

`bk_solution_ai` 应用通过 AVDK VFS 把 SD card drive 1 挂到 `/sd0`：

```c
partition.part_type = FATFS_DEVICE;
partition.part_dev.device_name = FATFS_DEV_SDCARD;
partition.mount_path = VFS_SD_0_PATITION_0;
mount("SOURCE_NONE", partition.mount_path, "fatfs", 0, &partition);
```

示例：

- `bk_solution_ai/components/bk_dual_screen_avi_player/bk_dual_screen_avi_player.c:81-103`
- `bk_solution_ai/components/network_transfer/volc_rtc/bk_volc_api.c:53-94`
- `bk_avdk_smp/ap/components/bk_vfs/fatfs_adapter.c:26-82`
- `bk_avdk_smp/ap/components/fatfs/disk_io.c:185-220`

OpenVela 不沿用该私有 mount 参数结构。目标使用 NuttX 原生块设备和 `vfat`：

```text
nx_mount("/dev/mmcsd0", "/mnt/sdnand", "vfat", 0, NULL)
```

若介质包含 MBR/GPT，则先注册分区块设备，再挂载 `/dev/mmcsd0p0`。具体选择必须由 sector 0 实测决定。

## 3. BK7258 硬件基线

### 3.1 SDIO 寄存器地址和安全属性

BK7258 SDIO Host secure base：

```text
SOC_SDIO_REG_BASE = 0x458d0000
```

依据：

- `bk_avdk_smp/ap/include/soc/bk7258/reg_base.h:19`
- `bk_avdk_smp/ap/include/soc/bk7258/reg_base.h:125`
- `bk_idk/include/soc/bk7258/reg_base.h:125`

当前 OpenVela AP 按 Secure 模式运行，首版使用 secure alias `0x458d0000`。在没有重新设计 TrustZone/PPC 前，不使用 non-secure alias `0x558d0000`。

已知寄存器范围至少为：

```text
0x458d0000 .. 0x458d005c
TX FIFO = base + 0x3c
RX FIFO = base + 0x40
```

依据：

- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sdio_reg.h:325-330`

生成头文件存在旧地址注释、CRC bit shift 冲突和 reset 语义冲突。实现必须优先使用经 AVDK V2 驱动验证的 LL 行为，并通过寄存器读回和实测波形确认；不能只复制生成注释。

### 3.2 中断

原厂定义：

```text
INT_SRC_SDIO = 10
CPU1 interrupt route bit = 10
```

依据：

- `bk_avdk_smp/ap/include/driver/int_types.h:98`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/icu_map.h:40`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sys_struct.h:523`

当前 NuttX BK7258 约定外部中断加 16，因此目标定义为：

```text
BK7258_EXTIRQ_SDIO = 10
BK7258_IRQ_SDIO    = 16 + 10 = 26
```

接入点：

- `board/beken/chips/bk7258/include/irq.h`
- `board/beken/chips/bk7258/hardware/bk7258_irq.h`
- `board/beken/chips/bk7258/bk7258_irq.c`

现有 `up_enable_irq()` 已能设置 CPU1 route bit 和 NVIC line。SDIO `attach()` 应使用标准 `irq_attach(BK7258_IRQ_SDIO, ...)`、清 pending 后 `up_enable_irq()`，不得另建一套 Armino interrupt registration ABI。

### 3.3 时钟、门控和 PWC 前置条件

原厂 Host 初始化执行：

1. 请求 `CLK_PWR_ID_SDIO` 上电/开钟。
2. 选择 XTAL 26 MHz 源。
3. 设置 SDIO 分频。
4. 打开 CPU1 SDIO interrupt route。
5. 配置控制器 interrupt mask。

依据：

- `bk_avdk_smp/ap/middleware/driver/sdio_host/sdio_host_driver.c:320-374`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sys_reg.h:271-275`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sys_reg.h:412-413`

关键限制：AP 的 `sys_drv_dev_clk_pwr_up()` 不是本地寄存器写，而是通过 PM Mailbox 请求 CPU0/CP 修改中央时钟门控：

- `bk_avdk_smp/ap/middleware/driver/sys_ctrl/sys_clock_driver.c:22-40`
- `bk_avdk_smp/ap/components/bk_pm/pm.c:1389-1400`
- `bk_avdk_smp/cp/middleware/driver/pwr_clk/low_pwr_core.c:228-259`

当前 OpenVela `bk7258_mailbox_send_pwc()` 发送基础请求后尚未完成 transport ACK 和 PM 语义响应等待，现有 PWM 代码也只能用固定延时规避时钟未就绪。SDIO 不能接受这种竞态，因为在 clock gate 未真正打开时写寄存器会导致初始化随机失败。

因此阶段一的前置任务是补齐可同步等待的 PWC clock-control helper：

```text
request CLK_PWR_ID_SDIO ON
  -> 等待 transport ACK
  -> 等待 CP PM completion/semantic response
  -> 读回中央 gate 或通过 SDIO version/register 验证可访问
  -> 再复位和配置 Host
```

若现有 CP wire ABI 没有独立语义响应，至少应以受控超时轮询 gate/readback 代替固定 sleep，并把超时返回给 bring-up。协议细节必须在编码前继续核对 CP PWC handler，不能猜测 command ID。

### 3.4 SDIO 时钟编码

AVDK 的 `sdio_host_clock_freq_t` 是寄存器编码，不是 Hz：

- `bk_avdk_smp/ap/include/driver/hal/hal_sdio_host_types.h:25-43`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/sdio_ll.h:92-96`

当前参考值：

| NuttX clock mode | BK 编码 | 预期 SDCLK |
| --- | ---: | ---: |
| `CLOCK_SDIO_DISABLED` | gate off | 0 |
| `CLOCK_IDMODE` | 7 | 26 MHz / 256 = 101.5625 kHz |
| `CLOCK_SD_TRANSFER_1BIT` | 14 | 320 MHz / 16 = 20 MHz |
| `CLOCK_SD_TRANSFER_4BIT` | 14，首版同速 | 20 MHz |

`CLOCK_MMC_TRANSFER` 可先映射到相同的安全传输时钟，但首版关闭 MMC support。示波器必须验证 ID 和 transfer 两种波形，因为 `sys_hal.h` 的通用分频注释与 SDIO 专用编码表存在冲突。

### 3.5 GPIO 和板级供电

`bk_solution_ai` 四个项目均将板载 SD-NAND 连接定义为：

| 信号 | GPIO | 首版是否使用 |
| --- | ---: | --- |
| SDIO CLK | GPIO14 | 是 |
| SDIO CMD | GPIO15 | 是 |
| SDIO DATA0 | GPIO16 | 是 |
| SDIO DATA1 | GPIO17 | 否，4-bit 阶段启用 |
| SDIO DATA2 | GPIO18 | 否，4-bit 阶段启用 |
| SDIO DATA3 | GPIO19 | 否，4-bit 阶段启用 |

依据：

- `bk_solution_ai/projects/beken_genie/ap/config/bk7258_ap/usr_gpio_cfg.h:36-41`
- `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/gpio_map.h:42-47`

参考项目中 CLK drive capacity 为 3，CMD/DAT 为 1，内部 pull disabled。这意味着板上可能有外部 pull-up，但源码不能证明。实板必须确认：

- CMD 和 DAT0..3 是否有外部上拉。
- 1-bit 模式下未使用的 DAT1..3 应保持输入/上拉还是继续复用为 SDIO。
- GPIO14..19 是否与 LCD、SPI0、PWM 或现有 Vela 功能冲突。
- 20 MHz CLK 的驱动能力和边沿质量。

`beken_genie`/`volc_rtc` 使用 `CONFIG_LDO3V3_CTRL_GPIO=52`，并在 AP 启动时把 GPIO52 拉高：

- `bk_solution_ai/projects/beken_genie/ap/config/bk7258_ap/config:1536`
- `bk_solution_ai/projects/beken_genie/ap/ap_main.c:144-149`

但 `ai_camera` 使用无效值 255，可能表示对应板型电源常开。当前 1 GB 板的 LDO GPIO、有效电平、上电延时和是否与其他外设共用均未由本地原理图证实。板级代码必须使用明确的 `BOARD_SDNAND_POWER_*` 定义，不得把 GPIO52 硬编码进芯片层。

### 3.6 容量预期

1 GB 商品容量通常按十进制计算：

```text
1,000,000,000 / 512 = 1,953,125 sectors
```

部分器件也可能报告接近 1 GiB：

```text
1,073,741,824 / 512 = 2,097,152 sectors
```

验收不得硬编码上述任一扇区数。应使用 CSD 解析和 `BIOC_GEOMETRY` 报告值，并只检查：

- sector size 为 512。
- `geo_nsectors * 512` 与器件规格、CID/CSD 和已知文件系统容量一致。
- LBA 超界访问被 NuttX block layer 拒绝或安全失败。

## 4. NuttX/OpenVela 接入模型

### 4.1 标准接口

NuttX 正式绑定 API：

```c
struct sdio_dev_s *sdio = bk7258_sdio_initialize(0);
ret = mmcsd_slotinitialize(0, sdio);
```

接口依据：

- `openvela/nuttx/include/nuttx/sdio.h:925-1045`
- `openvela/nuttx/include/nuttx/mmcsd.h:151-165`
- `openvela/nuttx/drivers/mmcsd/mmcsd_sdio.c:4548-4648`

成功后注册：

```text
/dev/mmcsd0
```

`sdio.h` 中提到的 `sdio_slotinitialize()` 是旧注释，实际 API 是 `mmcsd_slotinitialize()`。

### 4.2 lower-half 操作映射

BK7258 `struct sdio_dev_s` 首版必须提供：

| operation | BK7258 实现职责 |
| --- | --- |
| `reset` | 停 SDCLK，清 IRQ/FIFO/command/data 状态，复位软件 wait/transfer 状态 |
| `capabilities` | 首版返回 `SDIO_CAPS_1BIT_ONLY`，4-bit 阶段改为 `SDIO_CAPS_4BIT` |
| `status` | 固定返回 `SDIO_STATUS_PRESENT`，不报告 write-protect |
| `widebus` | 只切换 Host 数据宽度；卡侧 ACMD6 由 MMCSD 上层完成 |
| `clock` | 映射 disabled、ID、1-bit transfer、4-bit transfer 到 BK 编码 |
| `attach` | attach IRQ26、清 pending、打开 CPU1 route 和 NVIC |
| `sendcmd` | 解析 NuttX decorated command，设置 command index、arg、response、CRC 和 data direction |
| `blocksetup` | 设置 block length、block count/total length 和 data timeout |
| `recvsetup` | 保存 RX buffer/length，配置 PIO RX 和完成/错误 IRQ |
| `sendsetup` | 保存 TX buffer/length，配置 PIO TX 和完成/错误 IRQ |
| `cancel` | 关闭 data IRQ，清 FIFO/状态，终止当前 wait 并恢复可发命令状态 |
| `waitresponse` | 有界等待 command complete/timeout/CRC error |
| `recvshortcrc` | 读取并校验 R1/R1b/R6/R7 |
| `recvlong` | 按 NuttX 期望顺序返回 R2 的 4 个 word |
| `recvshort` | 读取 R3 OCR；R4/R5 使用非空 stub 返回 `-ENOSYS` |
| `waitenable` | 清 stale event，先于命令/数据启动 arm event mask 和 timeout |
| `eventwait` | 等待 ISR 发布 transfer done、timeout 或 error，处理先完成后等待竞态 |
| `callbackenable` | 固定介质可记录/忽略 media event mask |
| `registercallback` | 保存 callback；固定介质正常不触发插拔事件 |

操作契约：

- `openvela/nuttx/include/nuttx/sdio.h:445-775`
- `openvela/nuttx/include/nuttx/sdio.h:925-1045`

### 4.3 命令编码注意事项

NuttX 传给 `sendcmd()` 的 `cmd` 不是裸命令号，而是包含以下装饰位：

```text
MMCSD_CMDIDX_MASK
MMCSD_RESPONSE_MASK
MMCSD_DATAXFR_MASK
MMCSD_MULTIBLOCK
MMCSD_STOPXFR
MMCSD_OPENDRAIN
```

定义：

- `openvela/nuttx/include/nuttx/sdio.h:79-87`
- `openvela/nuttx/include/nuttx/sdio.h:243-289`

lower-half 必须从装饰值解析 command index、响应类型和数据方向，不能直接把整个 `cmd` 写入硬件，也不能只用裸 index switch 后忽略 response CRC 规则。

### 4.4 IRQ 和 event 状态机

建议私有状态至少包含：

```c
struct bk7258_dev_s
{
  struct sdio_dev_s dev;
  sem_t waitsem;
  struct wdog_s waitwdog;
  volatile sdio_eventset_t waitevents;
  volatile sdio_eventset_t wakeevent;
  uint8_t *buffer;
  size_t remaining;
  bool receive;
  sdio_event_callback_t callback;
  void *cbarg;
};
```

实际命名服从代码风格，核心要求是避免以下竞态：

```text
waitenable()
  -> clear stale status
  -> set waitevents
  -> arm IRQ/watchdog
recvsetup()/sendsetup()
sendcmd()
ISR may complete here
eventwait()
  -> 若 wakeevent 已设置，直接消费
  -> 否则等待 semaphore
```

ISR 只做：

- 读取并保存 interrupt status。
- 对 RX/TX FIFO 做有限 PIO 搬运。
- 将硬件错误映射为 `SDIOWAIT_ERROR`/`SDIOWAIT_TIMEOUT`。
- 将完成映射为 `SDIOWAIT_TRANSFERDONE`。
- 唤醒等待线程。

ISR 不做 FAT、mount、动态内存分配、长时间 busy wait 或 PM Mailbox 同步请求。

### 4.5 结构参考

首选 NuttX lower-half 结构参考：

- `openvela/nuttx/arch/arm/src/lpc17xx_40xx/lpc17_40_sdcard.c`
- 私有状态：`lpc17_40_sdcard.c:220-262`
- vtable：`lpc17_40_sdcard.c:404-448`
- constructor：`lpc17_40_sdcard.c:2711-2849`

如需参考 blocksetup 和 event race，可阅读：

- `openvela/nuttx/arch/arm/src/lpc54xx/lpc54_sdmmc.c:342-384`
- `openvela/nuttx/arch/arm/src/lpc54xx/lpc54_sdmmc.c:1630-1657`
- `openvela/nuttx/arch/arm/src/lpc54xx/lpc54_sdmmc.c:2148-2324`

只参考 NuttX 接口组织和并发模型，不复制其他芯片寄存器、FIFO 深度、时钟或 DMA 实现。

## 5. 目标代码布局

建议最小新增和修改范围：

```text
board/beken/chips/bk7258/
├── Kconfig                         # BK7258_SDIO，后续 DMA 选项
├── CMakeLists.txt                  # 条件加入 bk7258_sdio.c
├── Make.defs                       # classic Make 同步加入
├── bk7258_sdio.c                   # NuttX struct sdio_dev_s lower-half
├── bk7258_sdio.h                   # bk7258_sdio_initialize() 原型
├── include/irq.h                   # IRQ26 定义
└── hardware/
    ├── bk7258_memorymap.h          # secure SDIO base
    ├── bk7258_sdio.h               # 寄存器 offset/bit，使用 getreg32/putreg32
    └── bk7258_sysctrl.h            # SDIO clock/route 所需字段

board/beken/boards/bk7258/bk7258-ap/
├── include/board.h                 # GPIO14..19、LDO、电源延时、mount path
├── src/CMakeLists.txt
├── src/bk7258_mmcsd.c              # host bind、分区探测和可选 mount
├── src/bk7258_bringup.c            # late init 调用
└── configs/nsh/defconfig           # MMCSD/FAT/调试配置
```

职责边界：

- `hardware/bk7258_sdio.h` 只定义寄存器，不含 NuttX 文件系统逻辑。
- `bk7258_sdio.c` 不包含板载 GPIO52 等板型信息。
- `bk7258_mmcsd.c` 不直接发 SD command，只绑定 lower-half 和挂载块设备。
- 电源 rail、pin group 和 mount path 放板级。
- CP clock request helper 若需要增强，应放已有 Mailbox/PWC 层，不在 SDIO driver 内重复 wire protocol。

## 6. Kconfig 和 defconfig 方案

### 6.1 芯片 Kconfig

建议：

```kconfig
config BK7258_SDIO
	bool "BK7258 SDIO host"
	select ARCH_HAVE_SDIO
	select SDIO_BLOCKSETUP
	help
		Enable the BK7258 SDIO host lower-half for the AP core.

config BK7258_SDIO_DMA
	bool "BK7258 SDIO DMA"
	depends on BK7258_SDIO
	select SDIO_DMA
	help
		Enable only after PIO, DMA reachability and cache handling are verified.
```

DMA 阶段只有在实现 `dmapreflight()` 后才选择 `ARCH_HAVE_SDIO_PREFLIGHT`。

### 6.2 首版 defconfig

建议起始配置：

```text
CONFIG_BK7258_SDIO=y
# CONFIG_BK7258_SDIO_DMA is not set

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

CONFIG_SCHED_HPWORK=y

CONFIG_FS_FAT=y
CONFIG_NSH_MMCSDMINOR=0

CONFIG_DEBUG_MEMCARD=y
CONFIG_DEBUG_MEMCARD_ERROR=y
CONFIG_DEBUG_MEMCARD_WARN=y
CONFIG_DEBUG_MEMCARD_INFO=y
```

说明：

- `CONFIG_SCHED_HPWORK` 满足 MMCSD media callback/work queue 依赖，即使固定介质不发生插拔。
- `CONFIG_FS_FAT` 对应 NuttX 原生 `vfat`，不是 Armino 的 `CONFIG_FATFS`。
- `CONFIG_FSUTILS_MKFATFS` 首轮可以不启用，避免误格式化；进入受控格式化测试阶段再启用。
- `CONFIG_MMCSD_MMCSUPPORT` 首版关闭，避免无必要的 MMC/eMMC probe 和 EXT_CSD 分区路径。
- 若日志量影响时序，稳定后关闭 `CONFIG_DEBUG_MEMCARD_INFO`，保留 error/warn 和 driver counters。

### 6.3 分区配置

先读取 sector 0，再选择一种路径：

| 介质布局 | 识别特征 | OpenVela 路径 |
| --- | --- | --- |
| FAT superfloppy | sector 0 是 FAT BPB/boot sector | 直接 mount `/dev/mmcsd0` |
| MBR + FAT | offset `0x1fe` 为 `0x55aa`，有有效 MBR partition entry | 开 `CONFIG_MBR_PARTITION`，注册 `/dev/mmcsd0p0` |
| GPT + FAT | protective MBR，LBA1 有 `EFI PART` | 开 `CONFIG_GPT_PARTITION`，注册 GPT partition |
| 未格式化/未知 | 无合法 BPB/MBR/GPT | 不自动写入，转人工确认和受控格式化 |

NuttX 不会自动把 MBR 分区注册成 `/dev/mmcsd0pN`。板级应使用 `parse_block_partition()` 和自定义 handler：

- `openvela/nuttx/include/nuttx/fs/partition.h:49-83`
- `openvela/nuttx/boards/risc-v/mpfs/common/src/mpfs_emmcsd.c:47-79`

## 7. 分阶段实施

### 阶段 S0：硬件和 golden 固件取证

目标：在修改 OpenVela 前固定当前 1 GB 板的事实。

操作：

1. 使用未修改的 `bk_solution_ai` golden AP 固件挂载 SD-NAND。
2. 记录 GPIO52 电平、SDCLK 上电时序、ID clock 和 transfer clock。
3. 导出 CMD8、ACMD41 OCR、CID、CSD、RCA、card type 和容量。
4. 读取 sector 0、LBA1 和 FAT BPB，但不写介质。
5. 记录 FAT volume label、总容量、free space、cluster size 和现有文件 hash。
6. 确认板型原理图中的 GPIO14..19、LDO GPIO、有效电平、外部 pull-up 和 rail sharing。
7. 确认 CPU0/CP 没有同时初始化或访问 SDIO。

交付物：

- 当前板型硬件连接表。
- golden CID/CSD/OCR 和 sector 0 dump。
- golden SDCLK 频率与命令时序。
- 数据备份和 hash 清单。

退出条件：

- 1 GB 容量来自实测而不是参考文档。
- 明确 superfloppy、MBR 或 GPT。
- 明确电源 rail 和 GPIO，不再依赖 GPIO52 假设。

### 阶段 S1：PWC 时钟和寄存器可访问性

目标：保证 CPU1 在每次 Host 配置前已经获得 SDIO clock。

操作：

1. 核对 CP 对 `CLK_PWR_ID_SDIO` 的 command ID、参数和 completion 语义。
2. 在现有 Mailbox/PWC 层增加同步 clock request helper。
3. 使用有限超时等待 ACK/completion 或可靠寄存器 readback。
4. 开钟后读取 SDIO register 默认值/version；关钟后验证 gate 状态。
5. 所有失败均返回负 errno，不用固定延时后假定成功。

退出条件：

- 连续冷启动 100 次均能确定 clock request 成功或明确超时。
- clock 未就绪时不会继续初始化 SDIO。
- 不影响现有 heartbeat、PWC ready、日志和 PWM。

### 阶段 S2：寄存器、GPIO、IRQ 和命令层

目标：完成 lower-half 的 reset、clock、attach、sendcmd、waitresponse 和 response reader。

操作：

1. 定义 secure base `0x458d0000` 和经核对的 register bits。
2. 定义 external IRQ10/NuttX IRQ26。
3. 配置 GPIO14/15/16 为 CLK/CMD/DAT0，固定介质上电后满足器件延时。
4. 实现 ID clock 和 20 MHz clock 切换。
5. 实现 command complete、response timeout、CRC error 和 R1/R2/R3/R6/R7 读取。
6. 在 host constructor 内建立 semaphore/watchdog/event 状态并完成 reset。
7. 用临时 debug counters 记录每类 IRQ 和错误，不在 ISR 大量打印。

调试顺序：

```text
CMD0 无响应
  -> CMD8 R7 = 0x1aa
  -> CMD55 R1 APP_CMD bit
  -> ACMD41 R3 OCR ready
  -> CMD2 R2 CID
  -> CMD3 R6 RCA
  -> CMD9 R2 CSD
```

退出条件：

- CMD8 和 OCR 与 golden 一致。
- CID/CSD 连续复位读取稳定。
- 拔掉/断开 SD-NAND 或关闭 rail 时，命令在有限时间内返回 `-ETIMEDOUT`，不死锁。
- IRQ26 只由 SDIO 触发，不影响其他中断。

### 阶段 S3：PIO 单块数据和 `/dev/mmcsd0`

目标：实现 512 字节单块 PIO 数据传输并由 NuttX 注册块设备。

操作：

1. 实现 `blocksetup`、`recvsetup`、`sendsetup`、`cancel`、`waitenable`、`eventwait`。
2. RX/TX PIO 按 FIFO ready 和剩余字节数搬运，处理非 4-byte 对齐尾部。
3. 正确映射 data timeout、CRC fail、FIFO underflow/overflow 和 transfer end。
4. `bk7258_sdio_initialize(0)` 返回完整 `sdio_dev_s`。
5. 板级调用 `mmcsd_slotinitialize(0, dev)`。
6. 保持 `CONFIG_MMCSD_MULTIBLOCK_LIMIT=1`。
7. 只读执行 geometry、sector 0 dump 和全盘抽样 hash。

退出条件：

- `/dev/mmcsd0` 稳定出现。
- `BIOC_GEOMETRY` 报告 512-byte sector 和正确容量级别。
- sector 0 与 golden dump 完全一致。
- 连续读取、随机读取和未对齐用户 buffer 测试不产生 CRC/data error。
- timeout/cancel 后下一条命令仍可执行，无需整机复位。

### 阶段 S4：分区识别和 FAT32 挂载

目标：无损挂载已有文件系统。

操作：

1. 根据 S0 结果决定直接设备或分区设备。
2. 创建 `/mnt/sdnand`。
3. 以 NuttX `vfat` 挂载，不使用 Armino `fatfs` mount 参数。
4. 首次挂载只读或仅执行读取操作。
5. 比对目录、文件大小和 hash。
6. 之后进行受控小文件 create/write/fsync/read/rename/unlink 测试。
7. 每次破坏性测试前备份并确保介质未被 USB MSC 或其他核同时访问。

退出条件：

- 现有 FAT32 可重复挂载和卸载。
- golden 文件 hash 一致。
- `fsync()`、`sync()`、unmount 后重新上电数据仍一致。
- 不存在 OpenVela 和 PC USB mass-storage 同时写同一 FAT 卷的场景。

### 阶段 S5：多块传输和可靠性

目标：解除单块限制并验证标准 CMD18/CMD25/CMD12 生命周期。

操作：

1. 移除或增大 `CONFIG_MMCSD_MULTIBLOCK_LIMIT`。
2. 测试 2、4、8、32、128 sectors 连续读写。
3. 验证 CMD12、R1b busy、写完成和后续 CMD13/card state。
4. 对读写方向切换、非连续 LBA、超时和 CRC 注入执行恢复测试。
5. 运行 FAT 文件压力测试和大文件 hash。

退出条件：

- 多块性能高于单块且无数据差异。
- 10,000 次随机块操作无永久死锁、越界写和 silent corruption。
- 任意一次错误后可通过 cancel/reset 恢复，或明确返回需要重枚举。

### 阶段 S6：4-bit 模式

目标：启用 DAT1..3 并提高吞吐量。

操作：

1. 从原理图确认 GPIO17..19 和 pull-up。
2. 确认 GPIO18/19 不再承担 PWM/LCD 等冲突功能。
3. lower-half capabilities 改为 `SDIO_CAPS_4BIT`。
4. 由 NuttX 上层发送 ACMD6 后，`widebus(true)` 切 Host 为 4-bit。
5. 先保持 20 MHz，不同时提升频率。
6. 重跑所有错误恢复、压力和掉电测试。

退出条件：

- 逻辑分析仪确认 DAT0..3 均有活动。
- 4-bit 吞吐显著提升，CRC error 不高于 1-bit 基线。
- 低温、低压或长时间压力条件下无新增错误。

### 阶段 S7：DMA，可选

目标：在 PIO 已稳定后降低 CPU 占用。

前置事实：当前 OpenVela AP RAM MPU 为 non-cacheable，首版没有 D-cache coherency 问题，但 DMA master 的 DTCM/SRAM/PSRAM 可达性和 alignment 仍须实测。AVDK dormant DMA 路径把 endpoint 固定声明为 DTCM 且缺少完整 cache maintenance，不能直接照搬。

操作：

1. 确认 DMA request mux、方向、FIFO 地址和 IRQ。
2. 建立 DMA 可访问内存范围和最小 alignment。
3. 实现 `dmarecvsetup`、`dmasendsetup` 和 `dmapreflight`。
4. 对不满足条件的 buffer 返回 `-EFAULT`，使用 bounce buffer 或 `CONFIG_FAT_DIRECT_RETRY`。
5. TX 在 DMA 前 clean，RX 在 DMA 后 invalidate；即使当前 non-cacheable，也保留未来 cache enable 的明确策略。
6. 只有实现 preflight 后才上报 `SDIO_CAPS_DMASUPPORTED`。

退出条件：

- 任意 4/8/16/32-byte alignment 测试均通过或按设计走 bounce/retry。
- DMA 和 PIO hash 一致。
- PSRAM、SRAM、DTCM buffer 的支持/拒绝行为有明确测试结果。
- CPU 占用下降且吞吐提升，无 cache corruption。

## 8. 文件系统和数据安全策略

### 8.1 首次接入只读

首次 NuttX 枚举后只执行：

- `BIOC_GEOMETRY`。
- sector 0、LBA1、末尾附近 sector 的读取。
- FAT mount 后 `ls`、读取和 hash。

禁止执行：

- `mkfatfs`。
- `dd` 写入整盘。
- block driver destructive test。
- 自动 fsck 修复。
- 自动格式化 fallback。

### 8.2 格式化策略

只有以下条件全部满足时才允许格式化：

1. 已完成整盘备份。
2. 已确认介质布局不再需要兼容 PC/原厂固件的既有分区。
3. 介质已卸载且 USB MSC 未导出。
4. 使用明确设备节点，不通过模糊路径选择。
5. 操作由开发者显式触发，不由 boot bring-up 自动执行。

需要时启用：

```text
CONFIG_FSUTILS_MKFATFS=y
```

格式化工具依据：

- `openvela/apps/fsutils/mkfatfs/mkfatfs.c:121-209`
- `openvela/apps/fsutils/mkfatfs/mkfatfs.c:244-334`

### 8.3 多主机互斥

参考文档说明 PC 可通过 USB 读写 SD-NAND。这通常意味着设备可能被导出为 USB Mass Storage。FAT 不支持两个独立主机同时读写同一卷。

正式产品必须固定所有权状态机：

```text
OpenVela mounted
  -> USB MSC 不可导出或只读

切换到 USB MSC
  -> sync
  -> unmount
  -> block cache flush/transfer stop
  -> 再把设备交给 USB

USB eject/disconnect
  -> 重新枚举或确认 media state
  -> 再由 OpenVela mount
```

第一阶段不实现 USB MSC，只确保没有其他核或 PC 同时访问。

### 8.4 掉电一致性

板载 NAND 有内部 FTL 和写缓存的可能。普通 FAT `fsync()` 只能保证数据写到块设备请求边界，不能证明 NAND 内部电源故障安全。

验收应包含：

- 写文件后 `fsync()`，再 unmount 和正常断电。
- 写入中随机断电，重启后检查文件系统和旧文件完整性。
- CMD12/CMD13/R1b busy 完成后再关闭 LDO。
- power key 路径先执行 storage quiesce，再请求系统掉电。
- 若器件 datasheet 规定 power-off busy time，按其最大值执行。

在没有器件型号和 datasheet 前，不声称具备掉电不丢数据能力。

## 9. 错误处理和可观测性

### 9.1 errno 映射

建议统一：

| 硬件/协议错误 | lower-half 返回或 event |
| --- | --- |
| command/response timeout | `-ETIMEDOUT` |
| response CRC/index error | `-EIO` |
| data CRC/FIFO error | `SDIOWAIT_ERROR`，上层转 `-EIO` |
| data timeout | `SDIOWAIT_TIMEOUT` |
| unsupported response/path | `-ENOSYS` 或 `-EINVAL` |
| DMA buffer 不可达/未对齐 | `-EFAULT` |
| clock/PWC 未就绪 | `-ETIMEDOUT` 或底层明确错误 |
| 介质未成功枚举 | `-ENODEV` |

所有 wait 必须有有限超时；任何错误路径都要能 cancel、清 pending、释放 semaphore/mutex 并允许后续恢复。

### 9.2 驱动统计

建议保留低成本 counters，可通过 debug API 或 procfs 输出：

```text
cmd_count
cmd_timeout
cmd_crc_error
rx_blocks
tx_blocks
data_timeout
data_crc_error
fifo_error
cancel_count
host_reset_count
power_cycle_count
last_cmd
last_status
last_error
```

ISR 内不打印逐块日志。高频日志会改变 FIFO/IRQ 时序，并继续占用当前 Mailbox UART transport。

### 9.3 已知源码风险

移植时必须显式处理，不照搬：

- AVDK 生成寄存器头中的旧 base 注释和 CRC bit shift 冲突。
- FIFO/state reset 的 active level 注释互相矛盾。
- AVDK LL 的时钟寄存器地址存在 secure absolute address 硬编码。
- AVDK AP GPIO map 检查 `CONFIG_PIN_SDIO_GROUP_0`，而 Kconfig 使用 group 1 命名。
- 原厂 PIO TX 把任意 buffer 强转为 `uint32_t *`，未完整处理 alignment。
- 原厂 `rw_sync()` 主要是发送 CMD12，不等同于设备内部缓存 flush。
- `ai_camera` 的 LDO GPIO255 路径可能静默 no-op。
- 部分 `bk_solution_ai` mount helper 在 mount 失败时仍设置 mounted flag。
- 当前 OpenVela PWC transport 尚未完整消费 ACK/语义响应。

## 10. 验证矩阵

### 10.1 构建验证

每阶段执行 OpenVela AP 构建：

```bash
cd /home/mi/vela_competition/contest

./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  --cmake distclean

./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  -e -Werror \
  --cmake \
  -j8
```

然后按 `固件构建步骤.md` 使用 `EXTERNAL_AP_BIN` 生成 `all-app.bin`，并比较 `nuttx.bin`、`build/openvela-ap.bin` 和 packager `app1.bin` 的 SHA256。

### 10.2 基础枚举

| 用例 | 预期 |
| --- | --- |
| 冷启动 | `/dev/mmcsd0` 出现，无 timeout |
| 软复位 | CID/CSD/容量与冷启动一致 |
| 连续 100 次启动 | 无随机 mount 失败 |
| LDO 未打开 | 有限时间返回错误，不 hard fault/死锁 |
| 无介质响应 | command timeout 可恢复 |
| geometry | sector size 512，容量约 1 GB |
| sector 0 | 与 golden dump 一致 |

### 10.3 块读写

先在备份后的专用测试分区或空闲 LBA 执行：

| 用例 | 范围 |
| --- | --- |
| 单块读 | 首、中、末尾 LBA |
| 单块写回读 | 固定 pattern、walking bit、PRNG |
| 多块 | 2/4/8/32/128 sectors |
| 随机 LBA | 至少 10,000 次 |
| 未对齐 buffer | offset 1/2/3/4/8/16/32 |
| 边界 | LBA0、last LBA、越界 LBA |
| 方向切换 | read -> write -> read |
| 错误恢复 | timeout/CRC/cancel 后继续 IO |

可选 NuttX block test：

```text
CONFIG_TESTING_CMOCKA=y
CONFIG_TESTING_DRIVER_TEST=y
CONFIG_BCH=y
cmocka_driver_block -m /dev/mmcsd0
```

该测试会写约 95% 设备，禁止对生产 FAT 卷或未备份整盘执行。

### 10.4 FAT 验证

| 用例 | 预期 |
| --- | --- |
| 只读 mount/list/hash | 与 golden 一致 |
| create/write/fsync/read | 内容一致 |
| rename/unlink | 重新挂载后一致 |
| 0-byte/小文件 | 正确 |
| 大文件 | 超过 FAT cluster 和 driver transfer 边界 |
| 多线程文件 IO | 无死锁、无交叉损坏 |
| 反复 mount/unmount | 无引用泄漏或 stale state |
| 空间耗尽 | 返回 ENOSPC，不损坏已有文件 |

可启用：

```text
CONFIG_TESTING_FSTEST=y
CONFIG_TESTING_FSTEST_MOUNTPT="/mnt/sdnand"
```

### 10.5 性能门槛

性能不是首轮退出条件，但应记录：

- 1-bit PIO sequential read/write throughput。
- 4-bit PIO throughput。
- 4-bit DMA throughput。
- 4 KiB random read/write IOPS。
- CPU utilization、IRQ rate 和 Mailbox log rate。

每次优化只改变一个维度。4-bit 和 DMA 不能同一提交同时开启，否则出现 corruption 时无法定位。

### 10.6 长稳和掉电

- 连续 24 小时循环读写和 hash。
- 1000 次 mount/unmount。
- 100 次冷启动。
- 受控断电测试，分别覆盖 idle、写入中、`fsync()` 后和 unmount 后。
- 高低工作电压、温度和系统高负载下重跑。
- 与 Wi-Fi、Bluetooth、音频、PWM、日志并发，确认 IRQ/时钟/GPIO 无冲突。

## 11. 完成标准

SDIO/SD-NAND 移植只有同时满足以下条件才算完成：

1. 代码只维护在比赛仓 `board/beken/`，没有复制或修改公共 NuttX MMCSD 源码。
2. `all-app.bin` 构建、打包和 AP hash 校验通过。
3. PWC clock request 有确定完成机制，不依赖固定延时碰运气。
4. IRQ26、secure base、GPIO 和电源 rail 均有源码及实板证据。
5. `/dev/mmcsd0` 枚举稳定，geometry 报告 512 字节扇区和当前 1 GB 器件真实容量。
6. FAT32 现有数据可无损挂载，未发生自动格式化。
7. 单块、多块、随机、边界和错误恢复测试通过。
8. 24 小时长稳、100 次冷启动和受控掉电达到产品门槛。
9. 若启用 4-bit，GPIO17..19、pull-up 和冲突已由原理图与波形确认。
10. 若启用 DMA，buffer 可达性、alignment、cache maintenance 和 fallback 已验证。
11. CPU0/CPU1/USB MSC 对存储介质有唯一所有权，不存在双主机同时写 FAT。
12. 驱动错误有有限超时、errno、统计和可恢复路径，无 ISR 长日志。

## 12. 待确认清单

实施前需要补齐以下板级事实：

| 项目 | 当前状态 | 确认方法 |
| --- | --- | --- |
| 1 GB SD-NAND 厂商和型号 | 未知 | BOM、丝印、原理图 |
| 协议版本和 high-capacity bit | 推测为 SD Memory | CMD8/ACMD41/CSD 实测 |
| 精确扇区数 | 未知 | `BIOC_GEOMETRY` 与 CSD |
| GPIO14..19 接线 | 参考工程支持，当前板待证实 | 原理图和逻辑分析仪 |
| CMD/DAT 外部 pull-up | 未知 | 原理图和阻值测量 |
| SD-NAND LDO GPIO | 参考板 GPIO52，当前板待证实 | 原理图和电压测量 |
| LDO 有效电平/上电延时 | 未知 | datasheet 和示波器 |
| rail 是否与 LCD 等共用 | 未知 | 原理图 |
| FAT 是 superfloppy/MBR/GPT | 未知 | sector 0/LBA1 dump |
| PC USB MSC 的所有权机制 | 未实现 | 原厂 USB 路径和产品需求 |
| 最高可靠 SDCLK | 参考为 20 MHz | datasheet、SI 和压力测试 |
| 4-bit 是否可用 | 引脚存在，最终配置未启用 | 原理图、ACMD6 和压力测试 |
| DMA 可访问内存范围 | 未确认 | DMA 实测和总线文档 |
| SDIO reset active level | 源码注释冲突 | TRM、register readback、波形 |
| PWC clock completion ABI | 当前 OpenVela 未闭环 | CP PWC handler 源码和抓包 |

## 13. 推荐提交拆分

为便于定位回归，建议按以下顺序独立提交：

1. `docs: add BK7258 SDIO porting plan`
2. `fix: complete AP PWC clock request handshake`
3. `feat: add BK7258 SDIO command lower-half`
4. `feat: add BK7258 SDIO PIO data path`
5. `feat: bind BK7258 SD-NAND to MMCSD`
6. `feat: mount BK7258 SD-NAND FAT volume`
7. `feat: enable BK7258 SDIO multiblock transfers`
8. `feat: enable BK7258 SDIO four-bit mode`
9. `feat: add BK7258 SDIO DMA support`

每个功能提交都应包含对应构建结果和实板验证记录。4-bit 与 DMA 必须分开提交。
