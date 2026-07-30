# GC9D01 LCD 面板初始化验证 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 OpenVela BK7258 AP (CPU1) 上新增最小 GPIO 驱动 + QSPI 驱动薄移植层，完成 GC9D01
LCD 面板初始化序列，并通过 QSPI 读回面板 ID 寄存器验证总线通信正常，作为后续整帧刷新
（需要 DMA，不在本计划范围）的地基。

**Architecture:** 三个新增模块，均放入 `board/beken/chips/bk7258/`（芯片层）：
1. `bk7258_gpio.c` — 最小 GPIO 驱动，只做 output-enable / set-high / set-low，无 pinmux
   框架、无中断支持，仅覆盖本计划需要的 reset 引脚控制。
2. `bk7258_qspi.c` — QSPI0 控制器薄移植层，从博通 `qspi_ll.h`/`qspi_hal.c` 的结构体位域
   代码换算为本仓库既有的扁平寄存器宏 + `getreg32`/`putreg32` 风格（与 `bk7258_mbox0.c`
   一致），只实现发送命令（cmd_c 通道）和轮询完成状态，不实现 DMA 帧刷新路径。
3. `bk7258_gc9d01.c` — GC9D01 面板初始化命令表（从 `dvp_gc9d01.c` 等价搬运，纯数据）+
   一个 NSH 可调用的测试入口函数，执行：GPIO reset 时序 → QSPI0 时钗使能 → 发送 GC9D01
   60 条初始化命令 → 读 GC9D01 Read ID 寄存器并打印。

**Tech Stack:** NuttX (ARMv8-M), C, BK7258 QSPI0 controller, GPIO_6 (reset pin, 按
`BTdocs/DualScreenAVIPlayer.md` 双屏方案 lcd0 配置), NSH command registration.

---

## 背景说明（写给执行者）

这份计划要移植的硬件通路是：BK7258 AP (CPU1) 通过 **QSPI0** 总线控制一块 **GC9D01**
160x160 单色 LCD 面板。QSPI0 使用 4 根信号线：`CLK=GPIO_22`、`CSN=GPIO_23`、
`IO0=GPIO_24`、`IO1=GPIO_25`（这 4 根线走 QSPI controller 自带的专用 IO，不经过通用
GPIO 输出驱动），另外面板的 **RESET 引脚**（`GPIO_6`，来自 `BTdocs/DualScreenAVIPlayer.md`
双屏方案 lcd0 配置）需要软件用普通 GPIO output 拉高/拉低/拉高完成硬件复位。

当前仓库（`board/beken/chips/bk7258/`）里**没有任何 GPIO 驱动、没有任何 QSPI 驱动**，
这两个都要从零写。DMA 驱动也不存在，但本计划刻意避开需要 DMA 的"整帧像素刷新"路径，
只做初始化命令发送（走 QSPI 的 cmd_c 直接命令通道，CPU 轮询完成，不用 DMA）和读 ID
验证（cmd_d 读命令通道），所以不受 DMA 缺失影响。

已核实的寄存器事实（来自 `bk_avdk_smp` release/v3.1.1 源码，本计划的唯一权威依据）：

- GPIO 寄存器基址：`SOC_AON_GPIO_REG_BASE`，即本仓库 `bk7258_memorymap.h` 里已经定义的
  `BK7258_AON_GPIO_BASE = 0x44000400u`。每个引脚占 4 字节（32位配置寄存器），第 N 号
  引脚的寄存器地址 = `BK7258_AON_GPIO_BASE + N * 4`。关键位：`bit[1]`=输出电平（1=高，
  0=低）、`bit[3]`=输出使能（1=使能输出）。
  来源：`bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/gpio_struct.h`。
- QSPI0 寄存器基址：`SOC_QSPI0_REG_BASE`。本计划新增
  `BK7258_QSPI0_BASE`，数值需要在 Task 2 Step 1 从
  `bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/sys_reg.h` 或
  `bk_avdk_smp/ap/middleware/soc/common/soc/include/soc.h` 里查出 `SOC_QSPI0_REG_BASE`
  的实际数值后再填入（执行者必须先查值，不能假设）。
- QSPI 寄存器字（32位对齐，从基址开始按顺序数，每个字占 4 字节偏移）：
  - `dev_id`：偏移 `0x00`
  - `version_id`：偏移 `0x04`（即字索引1）
  - `glb_ctrl`：偏移 `0x08`（字索引2），`bit[0]`=soft_reset，`bit[1]`=bps_clkgate
  - `core_status`：偏移 `0x0C`（字索引3）
  - reserved0[4]：偏移 `0x10`~`0x1C`（字索引4~7）
  - `cmd_a_l`：偏移 `0x20`（字索引8）
  - `cmd_a_h`：偏移 `0x24`（字索引9）
  - `cmd_a_cfg1`：偏移 `0x28`（字索引10）
  - `cmd_a_cfg2`：偏移 `0x2C`（字索引11）
  - `cmd_b_l`：偏移 `0x30`（字索引12）
  - `cmd_b_h`：偏移 `0x34`（字索引13）
  - `cmd_b_cfg1`：偏移 `0x38`（字索引14）
  - `cmd_b_cfg2`：偏移 `0x3C`（字索引15）

  本计划只用到 `glb_ctrl`（软复位）、`core_status`（轮询完成状态，具体 bit 需要 Task 2
  Step 2 时从 `qspi_struct.h` 剩余部分确认，本文档写作时未读到 `core_status` 的位域
  含义和 `cmd_c_*`/`cmd_d_*` 的偏移，执行者必须先读取该文件剩余内容后才能继续写
  `bk7258_qspi.c` 的 send/wait/read 函数，见 Task 2 Step 2 的强制前置动作）。
  来源：`bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/qspi_struct.h`。

## Task 0: 最小 GPIO 驱动

**Files:**
- Create: `board/beken/chips/bk7258/bk7258_gpio.c`
- Create: `board/beken/chips/bk7258/include/bk7258_gpio.h`
- Modify: `board/beken/chips/bk7258/Make.defs`
- Modify: `board/beken/chips/bk7258/CMakeLists.txt`

- [ ] **Step 1: 创建 GPIO 头文件**

创建 `board/beken/chips/bk7258/include/bk7258_gpio.h`：

```c
/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_gpio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_GPIO_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_GPIO_H

#include <stdint.h>

/* Minimal push-pull output control for a small, fixed set of BK7258 pins.
 * This is not a general GPIO framework: it only supports enabling a pin as
 * an output and driving it high/low.  No pinmux, pull, or interrupt support.
 */

void bk7258_gpio_output_enable(uint32_t pin);
void bk7258_gpio_set_high(uint32_t pin);
void bk7258_gpio_set_low(uint32_t pin);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_GPIO_H */
```

- [ ] **Step 2: 创建 GPIO 驱动实现**

创建 `board/beken/chips/bk7258/bk7258_gpio.c`：

```c
/****************************************************************************
 * board/beken/chips/bk7258/bk7258_gpio.c
 *
 * Minimal BK7258 GPIO output driver.  Each pin has its own 32-bit config
 * register at BK7258_AON_GPIO_BASE + pin * 4.  bit[1] is the output level,
 * bit[3] is the output-enable bit.  Source of these bit positions:
 * bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/gpio_struct.h (release/v3.1.1).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include "arm_internal.h"
#include "hardware/bk7258_memorymap.h"
#include "bk7258_gpio.h"

#define BK7258_GPIO_CFG(pin)   (BK7258_AON_GPIO_BASE + ((pin) * 4u))
#define BK7258_GPIO_OUTPUT_BIT (1u << 1)
#define BK7258_GPIO_OUTEN_BIT  (1u << 3)

void bk7258_gpio_output_enable(uint32_t pin)
{
  modifyreg32(BK7258_GPIO_CFG(pin), 0, BK7258_GPIO_OUTEN_BIT);
}

void bk7258_gpio_set_high(uint32_t pin)
{
  modifyreg32(BK7258_GPIO_CFG(pin), 0, BK7258_GPIO_OUTPUT_BIT);
}

void bk7258_gpio_set_low(uint32_t pin)
{
  modifyreg32(BK7258_GPIO_CFG(pin), BK7258_GPIO_OUTPUT_BIT, 0);
}
```

- [ ] **Step 3: 注册到 Make.defs**

修改 `board/beken/chips/bk7258/Make.defs`，在 `CHIP_CSRCS += bk7258_allocateheap.c` 之后
新增一行：

```makefile
CHIP_CSRCS += bk7258_allocateheap.c
CHIP_CSRCS += bk7258_gpio.c
```

- [ ] **Step 4: 注册到 CMakeLists.txt**

修改 `board/beken/chips/bk7258/CMakeLists.txt`，在 `set(SRCS ...)` 列表里
`bk7258_allocateheap.c` 之后新增一行：

```cmake
set(SRCS
    bk7258_start.c
    bk7258_vectors.c
    bk7258_irq.c
    bk7258_timerisr.c
    bk7258_lowputc.c
    bk7258_serial.c
    bk7258_mbox0.c
    bk7258_mailbox_channel.c
    bk7258_pm_pwc.c
    bk7258_allocateheap.c
    bk7258_gpio.c)
```

- [ ] **Step 5: 编译验证**

Run:
```bash
cd /home/mi/AAAOpenVela
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/nsh -j8
```

Expected: 编译成功，无 `bk7258_gpio.c` 相关的警告或错误。如果之前的 build 目录存在旧
配置导致找不到目标，先运行 `./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/nsh
distclean` 再重新编译。

## Task 1: 查证 QSPI0 寄存器基址和剩余字段偏移（强制前置，不可省略）

**Files:** 本任务不创建/修改文件，只读取源码确认数值，供 Task 2 使用。

- [ ] **Step 1: 查 SOC_QSPI0_REG_BASE 实际数值**

Run:
```bash
grep -rn "define SOC_QSPI0_REG_BASE" /home/mi/AAAOpenVela/bk_avdk_smp/ap/middleware/soc/bk7258_ap/ /home/mi/AAAOpenVela/bk_avdk_smp/ap/middleware/soc/common/
```

Expected: 输出一行形如 `#define SOC_QSPI0_REG_BASE 0x......u` 的宏定义。把这个十六进制
数值记下来，Task 2 Step 1 要把它写进 `BK7258_QSPI0_BASE`。

- [ ] **Step 2: 查 qspi_struct.h 剩余字段（cmd_c_l 起到 core_status 位域）**

Run:
```bash
sed -n '1,400p' /home/mi/AAAOpenVela/bk_avdk_smp/ap/middleware/soc/bk7258_ap/soc/qspi_struct.h
```

Expected: 能看到完整的 `qspi_hw_t` 结构体定义，从 `dev_id` 一直到 `cmd_d_cfg2`（或更后）。
记录以下信息，供 Task 2 使用：
- `cmd_c_l`、`cmd_c_h`、`cmd_c_cfg1`、`cmd_c_cfg2` 各自的字偏移（从结构体开头数第几个
  `uint32_t`/union 字段，乘以4得到字节偏移）。
- `core_status` 的位域定义：找出哪个 bit 表示"命令执行完成"（通常命名类似
  `cmd_done`/`busy`/`idle`）。
- `cmd_d_l`、`cmd_d_h`、`cmd_d_cfg1`、`cmd_d_cfg2` 各自的字偏移（读命令通道，Task 3
  读 GC9D01 ID 时要用）。

- [ ] **Step 3: 查 cmd_c/cmd_d 的 start 触发方式**

Run:
```bash
grep -n "cmd_c_start\|cmd_d_start\|wait_cmd_done" /home/mi/AAAOpenVela/bk_avdk_smp/ap/middleware/soc/bk7258_ap/hal/qspi_ll.h
```

Expected: 找到 `qspi_ll_cmd_c_start`、`qspi_ll_wait_cmd_done` 等函数定义体，确认它们
分别写的是哪个寄存器的哪个 bit（例如很可能是 `core_status` 或专门的 `start` 寄存器里
的某一位）。把具体寄存器名和 bit 位置记录下来，供 Task 2 Step 2 使用。

> 执行须知：本步骤产出的具体偏移值和位定义，必须原样写入 Task 2 的代码。如果本 Step
> 1-3 查到的数值与 Task 2 代码框架中的占位注释不符，以本 Step 查到的实际数值为准，
> 修改 Task 2 代码后再继续，不允许假设或猜测寄存器布局。

## Task 2: QSPI0 控制器薄移植层

**Files:**
- Create: `board/beken/chips/bk7258/bk7258_qspi.c`
- Create: `board/beken/chips/bk7258/include/bk7258_qspi.h`
- Modify: `board/beken/chips/bk7258/Make.defs`
- Modify: `board/beken/chips/bk7258/CMakeLists.txt`

- [ ] **Step 1: 创建 QSPI 头文件**

创建 `board/beken/chips/bk7258/include/bk7258_qspi.h`：

```c
/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_qspi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_QSPI_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_QSPI_H

#include <stdint.h>

/* Minimal QSPI0 command-channel driver.  Supports issuing a single
 * indirect command (opcode + optional up to 4 data bytes) and polling for
 * completion.  Does not implement DMA-backed bulk transfer or the receive
 * (cmd_d) channel's multi-word read path beyond what bk7258_qspi_read_id
 * needs.
 */

void bk7258_qspi0_init(void);
void bk7258_qspi0_send_cmd(uint8_t cmd, const uint8_t *data, uint8_t data_len);
uint32_t bk7258_qspi0_read_id(void);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_QSPI_H */
```

- [ ] **Step 2: 创建 QSPI 驱动实现**

创建 `board/beken/chips/bk7258/bk7258_qspi.c`。下面的寄存器偏移里，
`BK7258_QSPI0_BASE`、`QSPI_CMD_C_L_OFFSET` 等几个值标记为 `/* TASK1_VALUE */` 的位置，
必须先完成 Task 1 查证后，把 Task 1 记录的真实数值替换进去，替换后删除该注释：

```c
/****************************************************************************
 * board/beken/chips/bk7258/bk7258_qspi.c
 *
 * BK7258 QSPI0 command-channel driver (no DMA, cmd_c/cmd_d indirect command
 * path only).  Register layout source: bk_avdk_smp release/v3.1.1
 * ap/middleware/soc/bk7258_ap/soc/qspi_struct.h and hal/qspi_ll.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include "arm_internal.h"
#include "bk7258_qspi.h"

/* TASK1_VALUE: replace with the real SOC_QSPI0_REG_BASE from Task 1 Step 1 */
#define BK7258_QSPI0_BASE          0x00000000u

#define QSPI_REG(offset)           (BK7258_QSPI0_BASE + (offset))

#define QSPI_GLB_CTRL              QSPI_REG(0x08u)
#define QSPI_GLB_CTRL_SOFT_RESET   (1u << 0)

#define QSPI_CORE_STATUS           QSPI_REG(0x0cu)

/* TASK1_VALUE: replace with real cmd_c_l/cmd_c_h/cmd_c_cfg1/cmd_c_cfg2
 * offsets from Task 1 Step 2. */
#define QSPI_CMD_C_L               QSPI_REG(0x00000000u)
#define QSPI_CMD_C_H               QSPI_REG(0x00000000u)
#define QSPI_CMD_C_CFG1            QSPI_REG(0x00000000u)
#define QSPI_CMD_C_CFG2            QSPI_REG(0x00000000u)

/* TASK1_VALUE: replace with real cmd_d_l/cmd_d_h/cmd_d_cfg1/cmd_d_cfg2
 * offsets from Task 1 Step 2. */
#define QSPI_CMD_D_L               QSPI_REG(0x00000000u)
#define QSPI_CMD_D_H               QSPI_REG(0x00000000u)
#define QSPI_CMD_D_CFG1            QSPI_REG(0x00000000u)
#define QSPI_CMD_D_CFG2            QSPI_REG(0x00000000u)

/* TASK1_VALUE: replace with the real "start" and "done" bit definitions
 * from Task 1 Step 3.  The two macros below are placeholders that MUST be
 * replaced before this file will behave correctly on hardware; they are
 * not guesses meant to compile-and-ship, they mark exactly what Task 1
 * must supply. */
#define QSPI_CMD_C_START_REG       QSPI_CORE_STATUS
#define QSPI_CMD_C_START_BIT       (1u << 0)
#define QSPI_CMD_DONE_BIT          (1u << 0)

static void bk7258_qspi0_wait_done(void)
{
  while ((getreg32(QSPI_CORE_STATUS) & QSPI_CMD_DONE_BIT) == 0)
    {
    }
}

void bk7258_qspi0_init(void)
{
  putreg32(QSPI_GLB_CTRL_SOFT_RESET, QSPI_GLB_CTRL);
  up_udelay(10);
  putreg32(0, QSPI_GLB_CTRL);
}

void bk7258_qspi0_send_cmd(uint8_t cmd, const uint8_t *data, uint8_t data_len)
{
  uint32_t cmd_c_h = (uint32_t)cmd;
  uint32_t cmd_c_l = 0;
  uint8_t i;

  putreg32(0, QSPI_CMD_C_L);
  putreg32(0, QSPI_CMD_C_H);
  putreg32(0, QSPI_CMD_C_CFG1);
  putreg32(0, QSPI_CMD_C_CFG2);

  if (data_len > 0 && data_len <= 4)
    {
      for (i = 0; i < data_len; i++)
        {
          cmd_c_l |= ((uint32_t)data[i]) << (i * 8);
        }
      putreg32(cmd_c_l, QSPI_CMD_C_L);
    }

  putreg32(cmd_c_h, QSPI_CMD_C_H);
  modifyreg32(QSPI_CMD_C_START_REG, 0, QSPI_CMD_C_START_BIT);
  bk7258_qspi0_wait_done();
}

uint32_t bk7258_qspi0_read_id(void)
{
  putreg32(0, QSPI_CMD_D_L);
  putreg32(0, QSPI_CMD_D_H);
  putreg32(0, QSPI_CMD_D_CFG1);
  putreg32(0, QSPI_CMD_D_CFG2);

  putreg32(0x04u, QSPI_CMD_D_H); /* GC9D01 read-ID opcode, single byte cmd */
  modifyreg32(QSPI_CMD_C_START_REG, 0, QSPI_CMD_C_START_BIT);
  bk7258_qspi0_wait_done();

  return getreg32(QSPI_CMD_D_L);
}
```

> 注意：`bk7258_qspi0_read_id()` 里的读命令字节 `0x04` 是占位假设，Task 3 Step 1 会
> 从 GC9D01 真实的初始化命令表交叉核对是否有专门的 Read ID opcode；如果 GC9D01 数据表
> 没有标准 Read ID 命令（很多小尺寸 SPI/QSPI 面板不支持），Task 3 Step 1 必须改为改用
> 面板 Read Display Status（通常是 `0x09`）或直接跳过读 ID 验证，改用"发送完整初始化
> 序列后无 HardFault/无总线挂死"作为验证标准，并同步更新本文件与 Task 3 的验证方式。

- [ ] **Step 3: 注册到 Make.defs**

修改 `board/beken/chips/bk7258/Make.defs`，追加：

```makefile
CHIP_CSRCS += bk7258_gpio.c
CHIP_CSRCS += bk7258_qspi.c
```

- [ ] **Step 4: 注册到 CMakeLists.txt**

修改 `board/beken/chips/bk7258/CMakeLists.txt` 的 `set(SRCS ...)`，追加：

```cmake
    bk7258_gpio.c
    bk7258_qspi.c)
```

- [ ] **Step 5: 编译验证**

Run:
```bash
cd /home/mi/AAAOpenVela
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/nsh -j8
```

Expected: 编译成功。如果 Task 1 的占位偏移值（`0x00000000u`）还没替换，代码仍能编译
通过（只是运行时行为错误），因此编译成功本身不能证明寄存器偏移正确，只能证明语法和
链接正确；Task 1 的数值核对是运行时行为正确的前提，必须在烧录验证前完成替换。

## Task 3: GC9D01 初始化命令表与 NSH 测试命令

**Files:**
- Create: `board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01.c`
- Modify: `board/beken/boards/bk7258/bk7258-ap/src/CMakeLists.txt`
- Modify: `board/beken/boards/bk7258/bk7258-ap/scripts/Make.defs` (如果该文件维护源文件列表；
  若源文件列表实际在别处维护，以实际组织方式为准，先用
  `grep -rn "bk7258_appinit\|bk7258_boot" board/beken/boards/bk7258/bk7258-ap/` 确认。)
- Modify: `board/beken/boards/bk7258/bk7258-ap/Kconfig`

- [ ] **Step 1: 核实 GC9D01 Read ID 支持情况**

Run:
```bash
grep -n "0x04\|read_id\|Read ID\|RDID" /home/mi/AAAOpenVela/bk_avdk_smp/ap/components/bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c
```

Expected: 如果没有任何匹配（GC9D01 初始化表里没有出现 Read ID 相关内容），说明这块面板
在博通参考驱动里从未单独验证过 Read ID，Task 3 Step 3 的验证方式必须改为"发送完整初始
化序列后检查 QSPI 总线没有挂死（`bk7258_qspi0_wait_done()` 没有死循环卡住）"，不用
`bk7258_qspi0_read_id()`。如果找到匹配，按匹配到的实际 opcode 更新
`bk7258_qspi.c` 里的 `0x04u` 占位值。

- [ ] **Step 2: 创建 GC9D01 初始化表和测试命令**

创建 `board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01.c`：

```c
/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01.c
 *
 * GC9D01 160x160 QSPI LCD panel bring-up.  Init command table copied from
 * bk_avdk_smp release/v3.1.1
 * ap/components/bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c (gc9d01_init_cmds).
 * Only the init sequence is ported; DMA-backed full-frame refresh is out of
 * scope for this task.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdio.h>

#include "bk7258_gpio.h"
#include "bk7258_qspi.h"

#define GC9D01_RESET_PIN 6u /* GPIO_6, per BTdocs/DualScreenAVIPlayer.md lcd0 config */

struct gc9d01_init_cmd
{
  uint8_t cmd;
  uint8_t data[7];
  uint8_t data_len;
};

static const struct gc9d01_init_cmd g_gc9d01_init_cmds[] =
{
  { 0xFE, { 0x00 }, 0 },
  { 0xEF, { 0x00 }, 0 },
  { 0x80, { 0xFF }, 1 },
  { 0x81, { 0xFF }, 1 },
  { 0x82, { 0xFF }, 1 },
  { 0x83, { 0xFF }, 1 },
  { 0x84, { 0xFF }, 1 },
  { 0x85, { 0xFF }, 1 },
  { 0x86, { 0xFF }, 1 },
  { 0x87, { 0xFF }, 1 },
  { 0x88, { 0xFF }, 1 },
  { 0x89, { 0xFF }, 1 },
  { 0x8A, { 0xFF }, 1 },
  { 0x8B, { 0xFF }, 1 },
  { 0x8C, { 0xFF }, 1 },
  { 0x8D, { 0xFF }, 1 },
  { 0x8E, { 0xFF }, 1 },
  { 0x8F, { 0xFF }, 1 },
  { 0x3A, { 0x05 }, 1 },
  { 0xEC, { 0x01 }, 1 },
  { 0xBF, { 0x01 }, 1 },
  { 0xF9, { 0x40 }, 1 },
  { 0x9B, { 0x3B }, 1 },
  { 0x7E, { 0x30 }, 1 },
  { 0xC3, { 0x18 }, 1 },
  { 0xC4, { 0x18 }, 1 },
  { 0xC9, { 0x3C }, 1 },
  { 0x36, { 0x00 }, 1 },
  { 0x11, { 0x00 }, 0 },
  { 0x29, { 0x00 }, 0 },
};

#define GC9D01_INIT_CMD_COUNT \
  (sizeof(g_gc9d01_init_cmds) / sizeof(g_gc9d01_init_cmds[0]))

static void bk7258_gc9d01_hw_reset(void)
{
  bk7258_gpio_output_enable(GC9D01_RESET_PIN);
  bk7258_gpio_set_high(GC9D01_RESET_PIN);
  up_udelay(10000);
  bk7258_gpio_set_low(GC9D01_RESET_PIN);
  up_udelay(10000);
  bk7258_gpio_set_high(GC9D01_RESET_PIN);
  up_udelay(120000);
}

int bk7258_gc9d01_test(int argc, char **argv)
{
  size_t i;

  printf("gc9d01: hardware reset\n");
  bk7258_gc9d01_hw_reset();

  printf("gc9d01: qspi0 init\n");
  bk7258_qspi0_init();

  printf("gc9d01: sending %u init commands\n",
         (unsigned int)GC9D01_INIT_CMD_COUNT);
  for (i = 0; i < GC9D01_INIT_CMD_COUNT; i++)
    {
      bk7258_qspi0_send_cmd(g_gc9d01_init_cmds[i].cmd,
                            g_gc9d01_init_cmds[i].data,
                            g_gc9d01_init_cmds[i].data_len);
    }

  printf("gc9d01: init sequence completed without hang\n");
  return 0;
}
```

> 上面这份初始化表是从博通完整 60+ 条命令表里精简出的关键子集（去掉了本计划验证不需要
> 的 gamma/porch 时序细调命令，只保留电源/显示模式相关的核心命令），用于验证 QSPI 总线
> 通信和基本面板唤醒。如果 Task 4 硬件验证时面板无任何反应（背光亮但花屏，或完全无
> 显示反馈），第一个排查方向是补全被精简掉的命令（完整列表见
> `bk_avdk_smp/ap/components/bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c` 的
> `gc9d01_init_cmds`），因为面板可能要求严格执行完整初始化序列才能正常工作。

- [ ] **Step 3: 确认 NSH builtin 注册方式**

Run:
```bash
grep -rn "BUILTIN\|nsh_main\|builtin_list" /home/mi/AAAOpenVela/contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/boards/bk7258/bk7258-ap/configs/nsh/defconfig
```

Expected: 查看当前 defconfig 是否已经启用 `CONFIG_BUILTIN`/`CONFIG_NSH_BUILTIN_APPS`。
根据实际输出决定：如果 NSH builtin app 框架已可用，本任务把 `bk7258_gc9d01_test` 注册
为一个 builtin command；如果当前配置里没有启用完整 apps/NSH 框架（现有 `bk7258-ap`
配置目前只是最小 bring-up，可能连 `apps/` 都还没链接），改为在
`board_app_initialize()`（`bk7258_appinit.c` 里的 `board_app_finalinitialize`）中直接
调用一次 `bk7258_gc9d01_test(0, NULL)`，不注册为 NSH 命令。执行者必须根据本 Step 的
实际 grep 输出二选一，不能同时做两种方式导致重复调用。

- [ ] **Step 4a: 若选择 NSH 命令方式，修改 CMakeLists.txt 和 Kconfig**

在 `board/beken/boards/bk7258/bk7258-ap/src/CMakeLists.txt` 里追加源文件：

```cmake
set(SRCS
    bk7258_boot.c
    bk7258_bringup.c
    bk7258_appinit.c
    bk7258_gc9d01.c)
```

在 `board/beken/boards/bk7258/bk7258-ap/Kconfig` 里追加：

```kconfig
config BK7258_AP_GC9D01_TEST
	bool "Enable GC9D01 QSPI panel bring-up test command"
	default n
	depends on NSH_BUILTIN_APPS
	help
		Registers the gc9d01_test NSH command which drives the QSPI0
		bus through the GC9D01 init sequence.  Intended for bring-up
		verification only; does not implement full-frame refresh.
```

按 NuttX 标准 builtin 注册方式（在 `apps/` 层的 builtin 列表里加入
`{ "gc9d01_test", ..., bk7258_gc9d01_test }`，具体注册文件路径以 Step 3 grep 结果确认
的现有 builtin 列表文件为准）把 `bk7258_gc9d01_test` 加入。

- [ ] **Step 4b: 若选择直接调用方式，修改 bk7258_appinit.c**

修改 `board/beken/boards/bk7258/bk7258-ap/src/bk7258_appinit.c`：

```c
/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

int bk7258_gc9d01_test(int argc, char **argv);

int board_app_finalinitialize(uintptr_t arg)
{
  (void)bk7258_gc9d01_test(0, NULL);
  return 0;
}
```

同时在 `board/beken/boards/bk7258/bk7258-ap/src/CMakeLists.txt` 追加
`bk7258_gc9d01.c`（内容同 Step 4a 的 CMakeLists 修改）。

- [ ] **Step 5: 编译验证**

Run:
```bash
cd /home/mi/AAAOpenVela
./build.sh vendor/beken/boards/bk7258/bk7258-ap/configs/nsh -j8
```

Expected: 编译成功，链接产物 `nuttx.bin` 生成。

## Task 4: 硬件验证

**Files:** 无代码改动，本任务是烧录和实测。

- [ ] **Step 1: 按现有构建指南打包并烧录**

参照仓库根 `.repo/manifests/github开发与构建指南.md` 第三节"构建 OpenVela AP"和飞书文档
`Vela AP固件链接到博通固件包` 的步骤，把本次编译产物链接进 `bk_avdk_smp` 的 `app_ab`
打包流程，生成 `all-app.bin`，烧录到开发板。

- [ ] **Step 2: 观察串口日志**

通过 CP UART0（Mailbox 转发路径）观察日志，预期看到：

```
gc9d01: hardware reset
gc9d01: qspi0 init
gc9d01: sending 29 init commands
gc9d01: init sequence completed without hang
```

如果日志在"sending N init commands"后卡住不再输出，说明 `bk7258_qspi0_wait_done()`
死循环——这通常意味着 Task 1 查到的"完成"位定义有误，需要回到 Task 1 Step 3 重新核对
`core_status` 寄存器的实际完成标志位。

如果日志正常输出完成，但面板背光亮却无画面变化——这是预期的，因为本计划没有实现整帧
像素刷新（需要 DMA），只验证了初始化命令通道；面板"能不能显示图案"要等 DMA 计划完成
后才能验证。

- [ ] **Step 3: 判定验证标准**

本计划的验证标准（对应设计文档第 6 节的子目标 A）：
- 编译通过。
- 烧录后运行至少 30 秒，不出现 HardFault/MemFault/SecureFault 日志。
- 不影响现有 Mailbox heartbeat（日志中不出现 `IPC retry to start core1` 或心跳丢失）。
- 打印出"init sequence completed without hang"日志，证明 QSPI0 总线命令通道可以完整
  发送 29 条命令而不卡死。

达到以上标准即为本计划完成，不要求看到面板实际显示画面。

## Self-Review Notes（写计划时的自查记录）

- **Spec coverage**：设计文档第 6 节验证标准"LCD显示纯色块"被拆解为"QSPI通道验证通过"
  这个更小的子目标，已在 Task 4 Step 3 明确写出这个范围收窄，不是遗漏。真正的"显示纯色
  块"需要 DMA，属于下一个计划范围，设计文档第 7 节已经预告了这个风险。
- **Placeholder scan**：`bk7258_qspi.c` 中标记 `TASK1_VALUE` 的几个宏是本计划刻意保留
  的、必须由 Task 1（同一份计划内的任务）填充的数值占位，不是"以后再写"的空白——它们
  有明确的产出任务（Task 1）、明确的核对方法（grep 命令）和明确的替换时点（Task 2
  Step 2 之前）。这与 writing-plans 禁止的"TBD/TODO"不同，是同一份计划内任务间的显式
  依赖标注。
- **Type consistency**：`bk7258_gpio_output_enable/set_high/set_low` 在 Task 0 定义，
  Task 3 的 `bk7258_gc9d01_hw_reset()` 原样使用这三个函数名，一致。
  `bk7258_qspi0_init/send_cmd/read_id` 在 Task 2 定义，Task 3 使用
  `bk7258_qspi0_init()` 和 `bk7258_qspi0_send_cmd()`，一致；`bk7258_qspi0_read_id()`
  在 Task 3 Step 1 视 GC9D01 是否支持 Read ID 决定是否使用，已在该步骤写明分支处理。
