# Beken AVDK 覆盖与构建基线

本目录保存 BK7258 OpenVela 最终固件构建所需的 Beken AVDK 覆盖文件，并作为
多人协作时 `bk_avdk_smp` 修改的同步记录。不要只修改工作区中的
`~/vela_competition/bk_avdk_smp`；可复现修改必须同时更新本目录及本文档。

## 1. 当前覆盖文件

| 比赛仓权威副本 | `bk_avdk_smp` 目标文件 | 作用 |
| --- | --- | --- |
| `cp/components/bk_cli/cli_misc.c` | `cp/components/bk_cli/cli_misc.c` | OpenVela 480 MHz 模式禁用 `bootcore` 对 CPU2 的 legacy start/stop |
| `cp/components/bk_cli/cli_main.c` | `cp/components/bk_cli/cli_main.c` | 注册 `ap_console` CLI 命令 |
| `cp/components/bk_cli/uart_debug/command_ate.c` | `cp/components/bk_cli/uart_debug/command_ate.c` | OpenVela SMP 模式拒绝 ATE 直接控制 AP 电源与睡眠 |
| `cp/components/bk_cli/shell_task.c` | `cp/components/bk_cli/shell_task.c` | UART0 CP_CLI/AP_CONSOLE owner 状态机、延迟切换和退出转义 |
| `cp/components/bk_pm/pm.c` | `cp/components/bk_pm/pm.c` | DVFS 错误传播、vote/cache rollback 和 CPU/PSRAM 联合 VDDDIG 下限 |
| `cp/components/bk_startup/system_main.c` | `cp/components/bk_startup/system_main.c` | CPU1/CPU2 语义化 reset helper、run-status readback |
| `cp/components/controller_if/cif_ipc.c` | `cp/components/controller_if/cif_ipc.c` | 保留 Wi-Fi IPC RX status callback ABI |
| `cp/components/controller_if/Kconfig` | `cp/components/controller_if/Kconfig` | `CONFIG_WIFI_VNET_AP_IPV4` 配置 |
| `cp/components/controller_if/cif_cntrl.c` | `cp/components/controller_if/cif_cntrl.c` | STA scan/country IPC、扫描完成通知和结果快照 |
| `cp/components/controller_if/cif_main.c` | `cp/components/controller_if/cif_main.c` | IPC 批处理、enqueue failure 返回和 command/data ownership |
| `cp/components/controller_if/cif_main.h` | `cp/components/controller_if/cif_main.h` | OpenVela command ID、批处理消息和 callback ABI |
| `cp/components/controller_if/cif_wifi_dp.c` | `cp/components/controller_if/cif_wifi_dp.c` | STA ARP/IPv4 分流和 direct-push pbuf ownership |
| `cp/components/lwip_intf_v2_1/lwip-2.1.2/port/net.c` | `cp/components/lwip_intf_v2_1/lwip-2.1.2/port/net.c` | CP STA IPv4/DHCP/ARP ownership 禁用 |
| `cp/components/wpa_supplicant-2.10/wpa_supplicant/notify.c` | `cp/components/wpa_supplicant-2.10/wpa_supplicant/notify.c` | STA connected/disconnected event 转发 |
| `cp/components/wpa_supplicant-2.10/wpa_supplicant/wpa_scan.c` | `cp/components/wpa_supplicant-2.10/wpa_supplicant/wpa_scan.c` | AP-owned IPv4 下重复 disconnect 抑制 |
| `cp/include/components/ap_console_bridge.h` | `cp/include/components/ap_console_bridge.h` | CP UART0 bridge 模式、写入和统计接口 |
| `cp/include/components/shell_task.h` | `cp/include/components/shell_task.h` | shell 与 bridge 的切换/链路事件接口 |
| `cp/include/components/system.h` | `cp/include/components/system.h` | CPU reset/readback 语义化接口 |
| `cp/include/driver/mailbox_channel.h` | `cp/include/driver/mailbox_channel.h` | transport event、诊断和 poll 接口 |
| `cp/include/driver/low_pwr_core.h` | `cp/include/driver/low_pwr_core.h` | OpenVela 最终 ready worker 事件 |
| `cp/include/driver/mb_uart_driver.h` | `cp/include/driver/mb_uart_driver.h` | MB_UART link event、状态和 poll 接口 |
| `cp/include/driver/pwr_clk.h` | `cp/include/driver/pwr_clk.h` | PSRAM V1、OpenVela lifecycle 与 PM 诊断接口 |
| `cp/middleware/driver/common/driver.c` | `cp/middleware/driver/common/driver.c` | MB_UART0 唯一 owner、LOG/RAW RX、1024 B TX ring 和 worker |
| `cp/middleware/arch/cm33/trap_base.c` | `cp/middleware/arch/cm33/trap_base.c` | 崩溃 fail-stop 使用语义化 CPU1/CPU2 reset hold |
| `cp/middleware/driver/mailbox/mailbox_channel.c` | `cp/middleware/driver/mailbox/mailbox_channel.c` | 完整 ACK 匹配、timeout/reset、probe 和诊断 |
| `cp/middleware/driver/mailbox/mb_ipc_cmd.c` | `cp/middleware/driver/mailbox/mb_ipc_cmd.c` | IPC power-up ready 门禁事件 |
| `cp/middleware/driver/mailbox/mb_ipc_heartbeat.c` | `cp/middleware/driver/mailbox/mb_ipc_heartbeat.c` | heartbeat/reset/down 自动回退事件 |
| `cp/middleware/driver/mailbox/mb_uart_driver.c` | `cp/middleware/driver/mailbox/mb_uart_driver.c` | 严格 DATA/STATE 校验、ACK 后推进和 link event |
| `cp/middleware/driver/mailbox/mbox0_adapter.c` | `cp/middleware/driver/mailbox/mbox0_adapter.c` | command/ACK/reset/sync 分离稳定 slot |
| `cp/middleware/driver/mailbox/mbox0_adapter.h` | `cp/middleware/driver/mailbox/mbox0_adapter.h` | 稳定 slot 完成和 quarantine 接口 |
| `cp/middleware/driver/mailbox/mbox0_drv.c` | `cp/middleware/driver/mailbox/mbox0_drv.c` | RX 资源门禁、FIFO poll 和 destination count |
| `cp/middleware/driver/mailbox/mbox0_drv.h` | `cp/middleware/driver/mailbox/mbox0_drv.h` | MBOX0 poll、ready callback 和 FIFO count API |
| `cp/middleware/driver/psram/psram_driver.c` | `cp/middleware/driver/psram/psram_driver.c` | PSRAM 初始化失败时关闭已启用的 power/clock |
| `cp/middleware/driver/pwr_clk/Kconfig` | `cp/middleware/driver/pwr_clk/Kconfig` | 默认关闭的 `CONFIG_OPENVELA_AP_480M` 开关 |
| `cp/middleware/driver/pwr_clk/low_pwr_core.c` | `cp/middleware/driver/pwr_clk/low_pwr_core.c` | PSRAM V1 真实 ret/version、PM readback 与 OpenVela ready worker |
| `cp/middleware/driver/pwr_clk/pwr_clk.c` | `cp/middleware/driver/pwr_clk/pwr_clk.c` | 两阶段 ready、完整 boot rollback、可取消 recovery 与 FINISH shutdown |
| `cp/middleware/driver/sys_ctrl/sys_ps_driver.c` | `cp/middleware/driver/sys_ctrl/sys_ps_driver.c` | 单目标 DVFS 切换与错误/cache 传播 |
| `cp/middleware/soc/bk7258/hal/sys_hal.c` | `cp/middleware/soc/bk7258/hal/sys_hal.c` | bus divider bit 6、寄存器 readback 与失败恢复 |
| `cp/middleware/soc/common/hal/include/sys_hal.h` | `cp/middleware/soc/common/hal/include/sys_hal.h` | VDDD/VDDDIG readback API |
| `projects/app_ab/cp/cp_main.c` | `projects/app_ab/cp/cp_main.c` | OpenVela boot transaction 启动与失败日志保留 |
| `projects/app_ab/cp/config/bk7258/config` | `projects/app_ab/cp/config/bk7258/config` | app_ab CP 启用 OpenVela 480 MHz lifecycle |
| `projects/app_ab/partitions/bk7258/ram_regions.csv` | `projects/app_ab/partitions/bk7258/ram_regions.csv` | BK7258 640 KiB Share SRAM 和 16 MiB PSRAM 布局 |

UART0 bridge 保持 `MB_UART0` 唯一初始化 owner。AP 输出 LOG 模式保留 `ap0:`
前缀、50 ms 半行刷新和有限重试；RAW 模式不改写字节。PC 到 AP 输入使用
1024 B 有界 ring 和部分写 worker，所有物理 UART0 输出仍经 shell queue 串行化。

`ram_regions.csv`采用原厂 `lvgl/img_decode`、`lvgl/freetype_font` 的 16 MiB
七区域布局：

```text
0x60000000..0x60700000  四个媒体 slab，共 7 MiB
0x60700000..0x60720000  CP PSRAM heap，128 KiB
0x60720000..0x60a00000  AP PSRAM heap，2.875 MiB
0x60a00000..0x61000000  AP PSRAM section，6 MiB
```

## 2. 同步到 SMP 工作树

从工作区根目录执行：

```bash
mirror=contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/bk_avdk_smp
for path in \
  cp/components/bk_cli/cli_misc.c \
  cp/components/bk_cli/cli_main.c \
  cp/components/bk_cli/uart_debug/command_ate.c \
  cp/components/bk_cli/shell_task.c \
  cp/components/bk_pm/pm.c \
  cp/components/bk_startup/system_main.c \
  cp/components/controller_if/cif_ipc.c \
  cp/components/controller_if/Kconfig \
  cp/components/controller_if/cif_cntrl.c \
  cp/components/controller_if/cif_main.c \
  cp/components/controller_if/cif_main.h \
  cp/components/controller_if/cif_wifi_dp.c \
  cp/components/lwip_intf_v2_1/lwip-2.1.2/port/net.c \
  cp/components/wpa_supplicant-2.10/wpa_supplicant/notify.c \
  cp/components/wpa_supplicant-2.10/wpa_supplicant/wpa_scan.c \
  cp/include/components/ap_console_bridge.h \
  cp/include/components/shell_task.h \
  cp/include/components/system.h \
  cp/include/driver/mailbox_channel.h \
  cp/include/driver/low_pwr_core.h \
  cp/include/driver/mb_uart_driver.h \
  cp/include/driver/pwr_clk.h \
  cp/middleware/arch/cm33/trap_base.c \
  cp/middleware/driver/common/driver.c \
  cp/middleware/driver/mailbox/mailbox_channel.c \
  cp/middleware/driver/mailbox/mb_ipc_cmd.c \
  cp/middleware/driver/mailbox/mb_ipc_heartbeat.c \
  cp/middleware/driver/mailbox/mb_uart_driver.c \
  cp/middleware/driver/mailbox/mbox0_adapter.c \
  cp/middleware/driver/mailbox/mbox0_adapter.h \
  cp/middleware/driver/mailbox/mbox0_drv.c \
  cp/middleware/driver/mailbox/mbox0_drv.h \
  cp/middleware/driver/psram/psram_driver.c \
  cp/middleware/driver/pwr_clk/Kconfig \
  cp/middleware/driver/pwr_clk/low_pwr_core.c \
  cp/middleware/driver/pwr_clk/pwr_clk.c \
  cp/middleware/driver/sys_ctrl/sys_ps_driver.c \
  cp/middleware/soc/bk7258/hal/sys_hal.c \
  cp/middleware/soc/common/hal/include/sys_hal.h \
  projects/app_ab/cp/cp_main.c \
  projects/app_ab/cp/config/bk7258/config \
  projects/app_ab/partitions/bk7258/ram_regions.csv
do
  cp "$mirror/$path" "bk_avdk_smp/$path"
done
```

同步后必须逐字节检查：

```bash
for path in \
  cp/components/bk_cli/cli_misc.c \
  cp/components/bk_cli/cli_main.c \
  cp/components/bk_cli/uart_debug/command_ate.c \
  cp/components/bk_cli/shell_task.c \
  cp/components/bk_pm/pm.c \
  cp/components/bk_startup/system_main.c \
  cp/components/controller_if/cif_ipc.c \
  cp/components/controller_if/Kconfig \
  cp/components/controller_if/cif_cntrl.c \
  cp/components/controller_if/cif_main.c \
  cp/components/controller_if/cif_main.h \
  cp/components/controller_if/cif_wifi_dp.c \
  cp/components/lwip_intf_v2_1/lwip-2.1.2/port/net.c \
  cp/components/wpa_supplicant-2.10/wpa_supplicant/notify.c \
  cp/components/wpa_supplicant-2.10/wpa_supplicant/wpa_scan.c \
  cp/include/components/ap_console_bridge.h \
  cp/include/components/shell_task.h \
  cp/include/components/system.h \
  cp/include/driver/mailbox_channel.h \
  cp/include/driver/low_pwr_core.h \
  cp/include/driver/mb_uart_driver.h \
  cp/include/driver/pwr_clk.h \
  cp/middleware/arch/cm33/trap_base.c \
  cp/middleware/driver/common/driver.c \
  cp/middleware/driver/mailbox/mailbox_channel.c \
  cp/middleware/driver/mailbox/mb_ipc_cmd.c \
  cp/middleware/driver/mailbox/mb_ipc_heartbeat.c \
  cp/middleware/driver/mailbox/mb_uart_driver.c \
  cp/middleware/driver/mailbox/mbox0_adapter.c \
  cp/middleware/driver/mailbox/mbox0_adapter.h \
  cp/middleware/driver/mailbox/mbox0_drv.c \
  cp/middleware/driver/mailbox/mbox0_drv.h \
  cp/middleware/driver/psram/psram_driver.c \
  cp/middleware/driver/pwr_clk/Kconfig \
  cp/middleware/driver/pwr_clk/low_pwr_core.c \
  cp/middleware/driver/pwr_clk/pwr_clk.c \
  cp/middleware/driver/sys_ctrl/sys_ps_driver.c \
  cp/middleware/soc/bk7258/hal/sys_hal.c \
  cp/middleware/soc/common/hal/include/sys_hal.h \
  projects/app_ab/cp/cp_main.c \
  projects/app_ab/cp/config/bk7258/config \
  projects/app_ab/partitions/bk7258/ram_regions.csv
do
  cmp -s "$mirror/$path" "bk_avdk_smp/$path" || exit 1
done
```

全部`cmp`都必须返回0。此门禁应在构建前执行；Kconfig 会把项目的最小
`projects/app_ab/cp/config/bk7258/config`输入 fragment 原地展开为完整生成配置，
因此不得声称构建后该输入仍与权威最小种子逐字节一致。

## 3. 正确构建链条

最终固件链条固定为：

```text
OpenVela nuttx.bin
  -> bk_avdk_smp/build/openvela-ap.bin
  -> EXTERNAL_AP_BIN
  -> package/tmp/app1.bin
  -> package/all-app.bin

Armino CP app.bin + Beken bootloader.bin
  ---------------------------------------> package/all-app.bin
```

修改以下任一输入后，不得直接做 SMP 增量构建：

- `ram_regions.csv`
- AP/CP `config`
- Kconfig 默认值
- 分区生成脚本
- SoC 链接脚本

原因是顶层 `partitions/ram_regions.h` 可以先更新，而 AP/CP CMake 构建目录中的
`armino/partitions/_build/ram_regions.csv`、生成头和预处理链接脚本仍可能保留旧值。
这会形成顶层 16 MiB、核内仍 8 MiB 的不一致产物。

先构建 OpenVela AP，并将输入放到 SMP 工作树：

```bash
cd ~/vela_competition/contest
./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  -e -Werror --cmake -j8

cp \
  cmake_out/bk7258-ap_nsh/nuttx.bin \
  ~/vela_competition/bk_avdk_smp/build/openvela-ap.bin
```

然后执行项目级 clean 和容器内完整构建：

```bash
cd ~/vela_competition/bk_avdk_smp
make -C projects/app_ab clean

podman run --rm \
  --userns=keep-id \
  -v "$PWD:/armino" \
  -w /armino \
  -e EXTRA_CFLAGS=-Werror \
  localhost/bekencorp/armino-idk:1.5 \
  make -C projects/app_ab bk7258 \
  SDK_DIR=/armino \
  EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin
```

`make -C projects/app_ab clean`只删除`projects/app_ab/build`，不会删除
`build/openvela-ap.bin`。仅安装 Podman 时不要使用内部调用 Docker 的`dbuild.sh`。

CP 构建后先对生成配置做门禁，再恢复最小输入 fragment：

```bash
generated=projects/app_ab/build/bk7258/app_ab/bk7258/config/sdkconfig.h
grep -q '^#define CONFIG_OPENVELA_AP_480M 1$' "$generated"
grep -q '^#define CONFIG_WIFI_VNET_AP_IPV4 1$' "$generated"
grep -q '^#define CONFIG_GPIO_DEFAULT_SET_SUPPORT 1$' "$generated"

mirror=../contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/bk_avdk_smp
cp "$mirror/projects/app_ab/cp/config/bk7258/config" \
  projects/app_ab/cp/config/bk7258/config
cmp -s "$mirror/projects/app_ab/cp/config/bk7258/config" \
  projects/app_ab/cp/config/bk7258/config
```

权威 config 是 7 行最小种子（包含首个空行和显式关闭 SARADC mailbox），完整生成配置只存在于 build 目录，
不回写比赛仓。

## 4. 构建验收

以下文件必须都反映同一份 16 MiB 布局：

```text
projects/app_ab/partitions/bk7258/ram_regions.csv
projects/app_ab/build/bk7258/app_ab/partitions/ram_regions.h
projects/app_ab/build/bk7258/app_ab/bk7258/armino/partitions/_build/ram_regions.csv
projects/app_ab/build/bk7258/app_ab/bk7258_ap/armino/partitions/_build/ram_regions.csv
projects/app_ab/build/bk7258/app_ab/bk7258/armino/bk7258/bk7258_out.ld
projects/app_ab/build/bk7258/app_ab/bk7258_ap/armino/bk7258_ap/bk7258_ap_out.ld
```

关键值应为：

```text
CONFIG_PSRAM_CAPACITY          0x01000000
CP_PSRAM_HEAP                 0x60700000 + 0x00020000
AP_PSRAM_HEAP                 0x60720000 + 0x002e0000
AP_PSRAM_SECTION              0x60a00000 + 0x00600000
PSRAM 最终边界                0x61000000
```

外部 AP 输入和最终打包输入必须完全一致：

```bash
sha256sum \
  ~/vela_competition/contest/cmake_out/bk7258-ap_nsh/nuttx.bin \
  build/openvela-ap.bin \
  projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin

cmp -s \
  build/openvela-ap.bin \
  projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
```

检查最终产物：

```text
projects/app_ab/build/bk7258/app_ab/package/all-app.bin
projects/app_ab/build/bk7258/app_ab/package/app_ab_crc.rbl
projects/app_ab/build/bk7258/app_ab/package/build_summary.txt
```

`build_summary.txt`中的 AP 内存统计来自 Armino AP 占位镜像，只用于检查生成的
内存区域边界；它不代表最终打包的 OpenVela AP 静态占用。OpenVela AP 的静态
内存应以 `nuttx.map`或`System.map`为准。

## 5. OpenVela PSRAM 接入状态

OpenVela AP侧已经完成以下代码和离线构建基线：

- 精确16 MiB、RW/XN/non-cacheable MPU映射。
- `0x60720000..0x609fffff`独立AP PSRAM heap。
- USER/AUDIO/ENCODE/DISPLAY四个独立媒体pool。
- 明确排除`0x60700000..0x6071ffff` CP heap的地址检查。
- AP owner区域保存/恢复型alias probe，包含4/8/12 MiB测试点。
- `0x60a00000..0x60ffffff`静态section链接保留；reset copy/clear未实现前强制0 B。

本覆盖中的CP `0x7`协议要求request `param3=PM_PSRAM_PROTO_V1`；response `param1`回显
ON/OFF state、`param2`返回真实PSRAM driver结果、`param3`回显version。CP BOOT与
OpenVela使用独立owner，初始化失败不置owner bit。`0xc`已提供frequency/divider、
speed、voltage、CPU power/reset和PSRAM owner只读诊断；device ID和actual capacity
仍未暴露，因此alias probe和整颗器件实板测试仍是容量门禁。AP board必须同步发送
V1 version并检查真实ret/version后才能联调；本CP侧任务未修改AP board代码。

PWC ready 使用两阶段契约：AP 的既有 `PM_CPU1_BOOT_READY_CMD=0x5` 仅解除 CP boot
worker 等待，不发布最终 PWC/bridge ready。新增 `PM_OPENVELA_READY_CMD=0x11` 由 mailbox
ISR 排入 low-power worker；`param1=1` 表示 AP 已完成 PSRAM/SMP commit，CP 只有在
boot stage 5、CPU1/CPU2 power、CPU2 hold、480 MHz clock 和 OpenVela PSRAM owner
全部通过 readback 后才进入 stage 6 并发布 PWC ready。`param1=0` 时 `param2` 必须是
真实负 errno，CP 执行 best-effort 完整 rollback。正常 recovery 的 INIT/拒绝会取消
shutdown 并恢复 boot vote；只有 FINISH 才进入 shutdown。CP 不实现 runtime DVFS/P1，
也不在 recovery INIT 路径直接停止 CPU2。

实板首次联调发现 AP 在 `arm_serialinit()` 的 scheduler-lock 上下文中等待新建 mailbox
worker，导致 worker 无法运行、物理 mailbox 未启动便进入 SMP，最终 CP 在 stage 4
收不到 `0x5`。当前 AP 已拆分初始化：early serial 阶段只初始化逻辑状态、物理 MBOX
IRQ 和 console，`AppBringUp` 再启动 mailbox/MB-UART worker 并验证 CPU0 affinity。
`0x5` 也已移到 PWC callback 注册和worker就绪后。早期版本曾用PWC `0x12`替代原厂
power-up/heartbeat，导致基础liveness依赖UART STATE和PWC `0x5` completion；该迁移方案
现已废止。AP恢复在HW_CTRL `0x10`发送原厂command 1/2，PWC仅保留`0x5`和`0x11`。
CP stage-4等待窗口由3秒放宽为10秒，失败时保留错误和rollback现场，不再由`cp_main`
断言触发整芯片循环复位；最终`0x11` commit条件不变。

第二次实板联调仍停在 stage 4，且 CP 未收到 AP IPC power-up。AP early serial 中
`bk7258_mb_uart_init()` 的初始化 `printf()` 发生在 idle stdio 建立前，现已移除。
同时在 AP_SPINLOCK 尾部 `0x2800ffe0..0x2800ffff` 加入不依赖 mailbox/console 的启动
轨迹；CP timeout 会打印 `AP trace=<magic> primary=<stage> secondary=<stage>` 和5个
detail word。magic 应为 `0x41505452`。primary 1..4 表示 CPU1入口至 `nx_start`，
10..14 表示 early serial，20..24 表示 CPU2启动/IPI，30..34 表示 late bring-up至
`0x5` transport ACK；secondary 1..5 表示 CPU2入口、private、NVIC、mailbox和idle。

共享轨迹首次返回 `primary=2, secondary=0`，证明 CPU1 已进入 `bk7258_start()`，但
未完成主核 private 初始化。根因是启动代码在清零 BSS 前调用 `modifyreg32()` 开启
CPACR，而 NuttX `modifyreg32()` 使用 BSS 中的 spinlock；AP-only reset 后保留 SRAM
可令该锁保持占用，CPU1 永久自旋。当前已改为与 Armino `SystemInitCpu0()` 一致的
无锁 CPACR 直接读改写。`__start` 同时清零全部 detail word；primary 5/6/7/8/9
分别表示 CPACR、data copy、BSS clear、runtime vector copy 和 atomic gate 初始化完成。

下一轮轨迹返回 `primary=22, secondary=5`，证明 CPU1 已释放 CPU2 reset，CPU2 已完成
private/NVIC/mailbox 初始化并进入 secondary idle，但主核没有观察到 online。原实现
在 online 发布前开启 CPU2 全局中断，且 online/boot-test flag 全部使用同一个 libc
atomic hwspinlock；CPU1 以480 MHz紧密轮询该非公平锁，可令 CPU2 的一次 store 永久
饥饿，原厂 Armino secondary scheduler handoff 也不会在发布 ready 前开启中断。当前
bootstrap 已改为专用 raw Mailbox `BOOT_NOTIFY/BOOT_PING` 和 volatile shared flag加
barrier，不再依赖通用 atomic gate；CPU2 顺序固定为interrupt stack、online、notify、
最后开中断。secondary 6/7/8/9分别表示这四个阶段。正常 scheduler/call IPI 仍使用
原 atomic pending 状态机；500 ms诊断轮询中的无界 `wfe` 也已移除。

再下一轮轨迹推进到 `primary=22, secondary=9`，证明 CPU2 已完成 stack、notify 和
全局中断开启，而 CPU1仍未完成启动握手。`primary=22` 还可能表示 CPU1在第一次
读取系统 tick或等待 CPU1 Mailbox IRQ 时停住；该阶段不能要求 arch timer 已可靠
递增，也不应依赖启动期 IRQ 调度或跨核共享 ready flag。当前握手因此改为完全由
硬件 FIFO证明：CPU2发送 `BOOT_NOTIFY`，CPU1屏蔽本核中断并主动 drain channel 1，
收到 notify 后向 CPU2发送 `BOOT_PING`，CPU2的 channel 2 IRQ回 `BOOT_ACK`，CPU1
轮询收到 ACK 后才返回 `up_cpu_start()`。CPU1只更新自己的本地状态，不再读取
CPU2写入的 online flag；DWT cycle counter和纯迭代上限共同保证 AP先于 CP的10秒
门限留下诊断。secondary stage 10表示 CPU2已收到 ping并发出 ACK。

该版本实板轨迹为 `primary=24, secondary=10`，确认双向 hardware FIFO handshake 已
完整成功，停点已移动到 CPU1恢复本核中断或紧随其后的 NuttX SMP启动收尾。CPU1
主动 drain channel 1时硬件仍可能保留 Mailbox NVIC pending；当前在恢复中断前显式
清除该 pending，并把窗口继续拆分：primary 25表示准备恢复中断，26表示首个 IRQ已
进入（detail[2]保存 IRQ号，15是 SysTick、79是 Mailbox），27表示恢复中断调用已
返回，28表示 `up_cpu_start()` 即将成功返回，30仍表示 board late bring-up入口。

细分后的实板轨迹为 `primary=26, secondary=10, detail[2]=0x0f`，精确确认 CPU1
永久停在恢复中断后的首个 SysTick。首 tick 的 scheduler/atomic路径会立即使用 libc
全局 atomic gate，而原 gate以 SRAM `LDAEX/STREX`实现。BK7258原厂 spinlock源码
明确说明芯片只有两个 SRAM exclusive monitor，并对三个活跃核心参与 exclusive
access的配置直接编译报错；当前 CP、AP CPU1和AP CPU2三核同时运行，AP exclusive
序列可能永久失败。当前 AP-only atomic gate已改为两个AP逻辑核之间的 Peterson锁，
只使用位于 inner-shareable non-cacheable AP_SPINLOCK区的普通对齐 word store和
barrier，不再占用 exclusive monitor。NuttX上层 atomic/hwspinlock API保持不变，
`.bk_spinlock`由4字节增至20字节。普通 `spinlock_t` 已启用
`CONFIG_TICKET_SPINLOCK`，其 `atomic_cmpxchg` 也统一经 `CONFIG_LIBC_ATOMIC_HWSPINLOCK`
进入该 gate。Peterson trylock 每次失败都会撤回本核 intent，同核递归进入使用 owner/depth
计数，避免首个 tick 与 CPU2 IPI 并发时把失败状态遗留在 gate 中。

Peterson gate版本实板仍返回相同的 `primary=26, detail[2]=0x0f`，因此 exclusive
monitor是必须消除的独立三核风险，但不是当前首 tick停点的充分解释。继续对照原厂
FreeRTOS SMP发现其固定 `configTICK_CORE=CPU0`，tick owner直接推进内核 tick；当前
NuttX配置却启用了 `CONFIG_TIMER_ARCH`，将SysTick包装为通用 timer lower-half，首 tick
的 watchdog时间查询又通过 `clock_systime_ticks()`回读正在执行的同一 SysTick
lower-half。当前已关闭 `CONFIG_TIMER_ARCH`，改为 logical CPU0专属的直接 SysTick ISR，
CPU2继续保持私有 SysTick关闭；生成配置和最终 map已确认不再包含 `timer_callback`、
`up_timer_gettick`或`systick_initialize`，系统时间恢复为 `g_system_ticks`。首 tick新增
detail[3]诊断：0表示尚未进入BK7258 timer ISR，1表示已进入并正在执行
`nxsched_process_timer()`，2表示timer核心处理已返回。

direct SysTick版本上一轮实板轨迹为
`primary=26, secondary=10, detail[2]=0x0f, detail[3]=1`；随后修复版已推进到
`detail[3]=2, detail[4]=a8000000`。最终反汇编确认
`nxsched_process_timer()`第一条调用是`clock_timer()`，而`clock_timer()`在更新64位
`g_system_ticks`前立即通过`atomic_load_4()`和`atomic_compare_exchange_4()`进入
`g_atomic_hwspinlock`，所以该日志只能确认首 tick停在首次atomic/seqlock之后，不能直接
断言已经进入scheduler的`enter_critical_section()`。修复版的`detail[3]=2`则证明
`nxsched_process_timer()`已完整返回；`detail[4]=a8000000`表示该次atomic gate已完整释放。
本轮对Peterson gate补充owner/depth、
失败intent回滚和首tick状态探针；`detail[4]`高字节含义为A1入口、A2同核递归、A3 owner
忙、A4发布intent、A5双核冲突、A6获得gate、A7递归unlock、A8完整释放。NuttX ticket
`spin_trylock_notrace()`也修复为使用私有expected变量，避免CAS失败改写`lock->owner`。
SMP raw kick继续使用1024次有界FIFO重试，并在发送失败时清除`g_ipi_kicked`；pending
bit仍保留，避免错误状态伪装成已发送。首 tick当前停在`clock_timer()`的首次atomic之前，
尚无证据表明已经运行到round-robin远端IPI路径。

以下是 2026-08-06 本次 CP lifecycle 修改后的 OpenVela AP 打包输入：

```text
cmake_out/bk7258-ap_nsh/nuttx.bin
bk_avdk_smp/build/openvela-ap.bin
bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
SHA256 af4334f41c3a8fe125ea3e4201a38c55a2a8905c1125938cb5f56dd8da8d7926
```

本轮使用官方容器、项目级 clean 和 `EXTRA_CFLAGS=-Werror` 完整重建后的最终包：

```text
all-app.bin SHA256
9cb3626ffc41518293a48c3a088185cd38621dbbc0728bb0bda7ac55b69af37e

app_ab_crc.rbl SHA256
2537425fc3ac8d4b80b474db1f16e772d37d9ea097cf23de477402caa225628d
```

本轮已验证 CP `-Werror` 编译、最终打包、生成配置门禁、16 MiB 分区门禁和 AP 输入
逐字节一致性；尚未对新增 `0x11` commit/abort、recovery INIT 取消和 FINISH shutdown
执行实板验证。

2026-08-07 针对实板 `primary=26, secondary=10, detail[2]=0x0f,
detail[3]=2, detail[4]=a8000000` 的修复：

- 首个 SysTick 不再在 `clock_initialize()` 内立即启动；只安装 handler 和
  reload，待 `up_cpu_start()` 完成 CPU2 handshake、`nx_smp_start()` 返回后，
  在 board bring-up 开始时启动，避免首 tick 抢占 `up_cpu_start()` 的 IRQ
  restore/exception return 窗口。
- `up_send_smp_sched()` 和 `up_send_smp_call()` 不再因为 CH2 FIFO 短时满而
  直接 `PANIC()`。pending bit 保留、`g_ipi_kicked` 在发送失败时清零，CPU0
  的后续 SysTick 会重试合并后的 raw kick；同一目标的多个通知仍合并为一个
  FIFO entry。
- CH2 raw kick 发送增加 `detail[4]` 探针：`B1` entry、`B2` 已有 kick、
  `B3` FIFO send 成功（低16位为重试次数）、`B4` 发送失败（低16位为 errno，
  `0xffff` 为 FIFO retry timeout）。
- CPU2 secondary 增加 liveness 阶段：`11` 已完成 `sched_note_cpu_started`、
  `12` 正在等待 CPU0 进入 idle loop、`13` 已开始 idle；Mailbox IRQ 阶段
  `14` 入口、`15` 收到 raw kick、`16` 退出。Mailbox 单次 IRQ 最多处理 8
  个 FIFO entry，避免持续输入造成无限排空循环。

2026-08-07 实板新轨迹为 `primary=31, secondary=16`，说明上述 SMP 修复已生效：
CPU2 已完成 raw-kick Mailbox IRQ，CPU2 启动函数和 `nx_smp_start()` 已返回，
AP 已进入 late bring-up 并完成 mailbox worker 创建。当前新卡点位于 UART0
RESET/STATE link probe 之后、`primary=32` 之前。根因是 CP CPU1 启动等待原先
在 10 秒内忙轮询 AON RTC，抢占了唯一执行 `bk_mb_uart_poll()` 的 `ap_console`
线程，CP 硬件 IRQ 虽然收到了 AP descriptor，但逻辑 STATE/ACK 没有被调度处理。
本轮 CP 等待循环每次迭代加入 1 ms RTOS delay；AP `bk7258_mailbox_send_wire()`
也不再把物理 FIFO `-EAGAIN` 伪装为发送成功，避免 UART transaction 永久 busy。
AP UART link 等待从 1 秒延长为 8 秒，与 CP 的 10 秒 CPU1 boot transaction
deadline 保持余量。

本轮实板进一步推进到：

```text
ap0: mailbox UART0 link ready
CP OpenVela PM stage=5
```

这证明AP/CP RESET/STATE link、AP的`PM_CPU1_BOOT_READY_CMD=0x5`以及CP接收路径均已
成功。随后出现的`IPC[1]heartbeat timeout`证明将`0x5`直接映射为`CORE_POWER_ON`
会在heartbeat source尚未可靠建立时提前武装CP看门狗。随后采用的PWC `0x12`翻译层
仍把基础liveness绑定在更复杂的PWC启动事务上，最新实板继续出现`IPC retry`。
当前协议因此恢复原厂职责：HW_CTRL command 1触发`CORE_POWER_ON`，command 2每2秒
刷新heartbeat；PWC `0x5`只表示SMP/PWC worker milestone，`0x11`表示PSRAM/clock/SMP
最终commit或rollback。

当时实板日志再次返回 `primary=31, secondary=16`，没有到达 `primary=32`；该次
失败发生在 AP UART0 STATE probe，尚未执行后续liveness消息。AP 启动等待现在每
100 ms 主动重发 STATE request，并主动 drain MBOX RX，覆盖初始 RESET/PROBING
跨 worker 唤醒丢失的窗口；probe 超时额外打印 MB-UART 统计，用于区分 ACK 丢失、
远端协议错误和本地 worker 未发送。进一步新增 CP stage-4 timeout 快照，输出
`mbox0_adapter` 的 `bad_sid/bad_length/bad_address/overflow` 和逻辑 channel 的
`busy/sequence/command/stale_ack/tx_fault`，用于区分物理接收丢包和逻辑 ACK 丢包。
CP 侧未改原厂 UART/IPC handler。

最新 timeout 快照为 `rx=33 bad_sid=0 bad_length=0 bad_address=0 overflow=0`，
证明 AP 报文已被 CP adapter 接收；`ch=76 (0x4c)` 解码为原厂
`MB_CHNL_SARADC`，`cmd=1` 是 SARADC operation notification。此前 OpenVela AP
未处理该 CP->AP channel，返回 COM_FAIL/造成物理 transaction 恢复，连带阻塞
UART0 STATE ACK。AP 现增加 `0x4c` SARADC request 的原厂 ACK 兼容：收到 START/END
且 `param1=IPC_SARADC_OP_REQ` 时返回 `ack_data1=IPC_SARADC_OP_ACK`，不改变 CP
原厂 SARADC/IPC 实现。

2026-08-07 针对上述 `MB_CHNL_SARADC` 与 UART0 probe 并发窗口的修复：

- AP logical transport 保留最后一次 ACK、active transaction、command header 和
  发送 errno，并按 bit 记录 ACK 拒绝原因：`1` 无 active transaction、`2` control
  不为 ACK_BOX、`4` channel 不匹配、`8` sequence 不匹配、`16` command 不匹配、
  `32` UART ACK payload 非法、`64` ACK state 非法。timeout/fifo/ACK pool 计数器
  通过 boottrace detail[3] 保留低8位 errno、次8位 timeout、再后两组 fifo-full 和
  ACK-overflow 计数。
- AP 物理 MBOX 的 `bad_source/bad_length/bad_address/descriptor_full` 通过
  boottrace detail[4] 保留，便于 CP stage-4 timeout 时区分 envelope 过滤与逻辑
  ACK 处理失败。
- CP 保存最后一次发出的 physical command header、收到的 ACK header 和 ACK data1，
  stage-4 快照额外打印 `AP logical raw cmd/ack/data1`，可直接比较 channel、sequence
  和 command 编码。
- CP 在释放 CPU1 reset 前 quiesce peer logical channel；CP->AP 的本地 UART STATE
  ACK 不再提前解除 quiesce，只有收到 AP `PM_CPU1_BOOT_READY_CMD=0x5` 才恢复普通
  logical transaction。这样 SARADC 保持原运行期行为，但不会在启动 probe 窗口抢占
  唯一 physical transaction。
- CP 输入 fragment 已按构建规则恢复为权威最小配置；本轮 AP、CP、完整打包均使用
  `EXTRA_CFLAGS=-Werror` 通过。

本轮构建产物哈希：

```text
AP raw SHA256
19ea4269285dfa4605ec70d34e4e239889b087d2c678920daad60b61376e2425

all-app.bin SHA256
d562c8244ea794fb5e03337366304dc7311c5fa2809b646bed70fd76f000d185

app_ab_crc.rbl SHA256
91e6e69739219128050c66b85001fb0a2dc608e5348b6f066daa20c485ff2360
```

上一轮最终包哈希（已过时，仅保留历史对照）：

```text
AP raw SHA256
f2985d218c6e6ae63d9e7f90f81ae3fd32c7e8f70322158dbf6d5bd9f06b97a4

all-app.bin SHA256
84b46d106870e8b817a958a202cf436a7b0e994f828d1ac1404e8d1e49321b94

app_ab_crc.rbl SHA256
364779a083d4366a858ce06059647d9f57aa9771c1c06eab378cf85c9ad5cb54
```

本轮新固件重点观察：

```text
secondary=12: CPU2 已完成 ACK/调度启动，尚未进入 idle loop
secondary=13: CPU2 已进入 idle loop
secondary=15: CPU2 收到首个 raw kick
secondary=16: CPU2 完成首个 raw kick 的 Mailbox IRQ 处理
detail[4]=b1xxxxxx: CPU0 进入 raw kick
detail[4]=b2xxxxxx: kick 已被现有 g_ipi_kicked 合并
detail[4]=b3xxxxxx: FIFO 写入成功
detail[4]=b4xxxxxx: FIFO 写入失败或 1024 次重试超时
```

2026-08-07 针对实板出现 `mailbox UART0 link ready` 但未出现 `nsh>` 的修复：

- `board_late_initialize()` 在 `bk7258_pwc_start()` 返回前不会创建 `nsh_main`；
  因此 `stage=5` 只证明 CP 执行了 `0x5` boot-ready handler，不证明 AP 已收到该
  PWC transport ACK。
- CP mailbox RX adapter 现在在上层 ACK 队列达到 `MB_ACK_QUEUE_LEN - 1` 时停止接收
  普通 command，为当前 command 保留一个 ACK 槽，避免先执行 PWC callback 再因 ACK
  队列满而丢 ACK，形成 CP 已到 stage=5、AP 永久等待 PWC ACK 的状态。
- AP link wait 在重发 RESET/STATE probe 前再次确认 link state，避免 TX worker 刚完成
  STATE ACK 后被等待线程把 READY 链路重新切回 PROBING。
- 后续实板稳定复现 CP `stage=5`、AP 无 PWC 成功或失败日志，说明 CP 已处理 `0x5`，
  但 AP 的同步 PWC wait 没有消费返回 ACK。`wait_channel()` 现在和 UART link wait 一样
  每 1 ms 主动 drain MBOX RX；timeout 前先输出完整 mailbox stats。这样 PWC transport
  不再依赖 `board_late_initialize()` 阶段的 Mailbox IRQ 调度时机。

2026-08-10 实板在上述主动 drain 后仍稳定停在 `stage=5`。AP 的 500 ms PWC timeout
会将 UART link 一并切入 recovery，因此 timeout 文本只进入 AP UART ring、无法转发到
CP；日志中约 8 秒后的 `IPC retry to start core1` 不是 AP 仍在等待 8 秒。进一步确认
CP physical adapter 的 stable ACK slot 只由后续 CP command ACK fence 释放；启动阶段的
流量主要是 AP->CP UART/PWC command，没有该 fence。早期 UART backlog 会耗尽 4 个 ACK
slot，CP 仍先执行 `0x5` callback 到达 `stage=5`，但无法发布对应 transport ACK。

本轮将 CP physical ACK pool 扩为 8 个；CP 收到下一条普通 AP command 时，利用 AP
logical transport 的单 active-transaction 串行约束作为精确 fence，立即释放此前 ACK
slot。目标 FIFO 连续空闲 20 ms 后的 quarantine 回收作为无后续 command 时的兜底。
heartbeat 启动重试现在同时输出 CP physical/logical mailbox diagnostics，若仍失败可直接
查看 `slot_busy`、`ack_slots`、`ack_retry` 和 raw header。AP PWC receive callback 原先在
logical mailbox 已持有 driver lock 时递归获取同一 SMP spinlock；该回调现复用调用者的
临界区，避免后续 PSRAM/ready response 到达时死锁。

2026-08-10 新诊断显示 physical ACK pool 已正常回收：`slot_busy=0`、`ack_full=0`、
`ack_retry=0`、`ack_slots=0`，但启动期仍有多次 `0x4c` SARADC transaction timeout 和
transport recovery。为隔离 PWC `0x5` 链路，app_ab CP 项目明确关闭
`CONFIG_SARADC_MB`，不再初始化或发送跨核 SARADC START/END command；CP 本地 ADC、
温度和 RF 校准保持启用。`saradc_notify.c` 恢复原厂状态，不再作为比赛仓覆盖文件。
CP 诊断保留最后 physical RX header、最后成功发布的 transport ACK header、ACK 计数和
AP boottrace 快照，用于排除 ADC 后继续定位其他 transport 问题。

2026-08-10 后续实板日志显示 AP 已复制到正确的 `0x5` ACK（`12041005`），但仍停在
`primary=33`。对照发送和接收时序后确认 AP transport 存在发送前置状态竞态：
`dispatch_locked()` 原先先发布 physical mailbox 指针，随后才设置 `active->busy=true`；
双核 CP 可能在 `bk7258_mbox_send()` 返回前立即发送 ACK，`handle_ack()` 会把该 ACK
判为 `no active transaction` 并丢弃，事务随后永久等待。现已在发布指针前设置
`active->busy`，并预分配 transaction order；发送失败时回滚 busy 状态。这样 ACK 从
物理发送返回前到达也能正确匹配当前 PWC/UART transaction。

2026-08-10 最新实板仍显示`mailbox UART0 link ready`、CP `stage=5`、AP
`primary=33`，8秒后继续`IPC retry to start core1`。该结果证明PWC `0x12`迁移未解除
legacy `CORE_STARTING`依赖，且基础liveness仍被前置PWC transaction阻断。本轮按
`bk_avdk_smp_original_backup`恢复原厂契约：AP在UART link ready后、PWC `0x5`前，通过
HW_CTRL `0x10`发送16字节command 1并等待600 ms完成ACK；独立worker每2秒发送command 2，
4字节payload固定使用原厂SWAP地址`0x2809f900`。CP仅删除PWC `0x12`定义和翻译handler，
原厂`mb_ipc_cmd.c`、`mb_ipc_heartbeat.c`保持不变。PWC worker在`0x5`前完成有界启动，
`0x5/0x11`继续承担OpenVela SMP/PSRAM/clock生命周期，不再承担power-up门控。

为避免恢复HW_CTRL后仍被PWC并发故障阻断，本轮同时收敛AP PWC可靠性：所有PWC TX
通过单一mutex覆盖send/wait，worker的semantic response不再与`0x5/0x11/0x7`同步请求
竞争同一logical channel；PWC RX软件队列满时返回`-EAGAIN`并暂不回transport ACK，
worker释放queue slot后主动kick physical RX重试，避免“已ACK但语义消息静默丢失”。

本轮clean `-Werror`构建和最终打包产物：

```text
OpenVela AP / build/openvela-ap.bin / tmp/app1.bin SHA256
e9a390dd9e3111193ac6c98692f62c8cda12a69081564c1a49f59346f4fd81ed

all-app.bin SHA256
69c271fe38c59f9c7fde6f491b298665589bdc3e82c2f2f6c8b347aa3eb34b9b

app_ab_crc.rbl SHA256
ebd4ab8638d5e25652f648783fa2b4691ad32023279ee333c467aaac76528c52
```

2026-08-11 实板首次验证原厂HW_CTRL恢复版时，UART link ready后立即出现
`HW_CTRL power-up service failed, error=-1`，而mailbox统计仍为`tx=8/rx=8`、last command
仍是UART `0x19`，没有HW_CTRL `0x10` transaction。`last_tx_error=1`对应`EPERM`：新建
heartbeat worker设置CPU0 affinity后尚未完成迁移，仍在AP logical CPU1上执行；physical
MBOX按设计拒绝CPU2向CP发送16字节descriptor。随后修复版返回`-EXDEV`，证明在该
heartbeat worker内等待迁移仍不可靠。当前实现让`bk7258_mailbox_send_wire()`只登记
logical transaction并唤醒已有CPU0-bound `mbox-v2` TX worker；物理descriptor只由该worker
调用`bk7258_mbox_send()`。创建mailbox worker前，AP将创建者临时固定到logical CPU0，
使所有kernel worker从首次调度起继承CPU0-only affinity。没有放宽CPU2 physical mailbox
权限，也没有修改CP command 1/2 ABI。

本轮最新版本另外完成了AP `distclean`、`-Werror`完整构建、CP clean容器构建和标准打包。
AP raw、`build/openvela-ap.bin`和`package/tmp/app1.bin`逐字节一致；以上SHA-256为本轮
最终产物，仍待实板确认HW_CTRL cmd1 ACK、PWC `0x5/0x11` commit和`nsh>`。

2026-08-11 下一轮实板已确认HW_CTRL cmd1和PWC `0x5`均成功，但CP heartbeat timestamp
仍停在cmd1时刻，约8秒后原厂`mb_ipc_task` assert并重启。进一步核对当前生成配置和
`System.map`后确认：本固件为`CONFIG_SMP=y`且未启用`CONFIG_BMP`，所以
`DEFINE_PER_CPU_BMP`在当前构建中退化为全局单实例；`g_system_ticks`和
`g_wdactivelist`不是此前推断的per-CPU对象。当前修复保持CPU0-only SysTick和完整
`nxsched_process_timer()`不变，删除未经实板证明会持续计数的AON RTC heartbeat时基；
cmd1 ACK后按原厂时序建立2秒NuttX tick deadline，不再紧接cmd1发送cmd2，后续cmd2由
CPU0-only `mbox-v2` TX worker每2秒调度。前三次提交和ACK输出限量诊断。原厂cmd2 ABI、
CP handler、watchdog和16 MiB PSRAM layout均未修改。

2026-08-11 实板验证上述版本时，四次启动均有cmd1 ACK和PWC `0x5` ACK，但完全没有
`heartbeat queued`，同时PWC在发送PSRAM power request后也没有触发1秒timeout，直到CP在
约8秒后复位。这证明故障位于cmd2提交之前，且不是heartbeat单一状态机问题，而是所有依赖
NuttX tick的超时和周期worker均未继续推进。根因是`bk7258_timer_start()`原先作为late
bring-up第一条操作执行，而bring-up线程要到后续`bk7258_mailbox_workers_start()`才设置
CPU0 affinity；非debug构建中的`DEBUGASSERT(up_cpu_index() == 0)`不能形成运行时保证，
因此私有SysTick可能在logical CPU1启动。

当前修复通过`nxsched_smp_call_single(0, ...)`让SysTick寄存器写和使能确定发生在logical
CPU0；bring-up和heartbeat启动线程也显式限制为CPU0。cmd1 ACK后的heartbeat enable和2秒
deadline由CPU0 mailbox完成回调设置，避免跨核发布竞态。AON RTC不属于当前heartbeat
实现，旧的secure地址常量仍未作为协议时基使用。完整clean `-Werror`构建和打包产物：

```text
OpenVela AP / build/openvela-ap.bin / tmp/app1.bin SHA256
987ffd1b0a38a02ce4fa07e75ce5f01d55abdda5d99b93002a9688f30db4edee

all-app.bin SHA256
102d46ac3d7c3c1f554d1070c5281dcc2be7b9167e508d79f9b19bde683c765d

app_ab_crc.rbl SHA256
96ac8b6b6e7f4efd92c5e7f9ee19bc5d2211485445ef2ca222c8727e440ab3bf
```

下一次实板必须先看到前三组`heartbeat queued`/`heartbeat ACK result=0`且30秒内无CP
heartbeat assert，再确认PSRAM response、PWC `0x11`、stage 6、`nsh>`。按键震动worker
在这些步骤和Wi-Fi初始化之后才创建；只有看到`power-key motor worker ready`后，按键/震动
故障才可作为独立GPIO/PWM问题分析。

2026-08-11 对`all-app.bin=102d46ac...`的实板验证显示该版发生启动回归：CP快照为
`primary=31, secondary=10`，UART STATE物理请求/ACK分别为`19020001/19021001`，证明
descriptor和匹配ACK已到AP，但CPU2仍停在boot ACK后的exception-return窗口，未到
secondary scheduler阶段。该版在late bring-up入口先改变初始化线程CPU affinity，再通过
`nxsched_smp_call_single(0, ...)`启动SysTick；两步都会在该敏感窗口触发SMP IPI，复现了
历史上的首tick/调度IPI过早抢占问题。因此`102d46ac...`已废弃，不得继续烧录。

当前修复不再从bring-up线程发送SMP call。在任何worker创建和父线程setaffinity之前，先只读
轮询boottrace直到`secondary >= 12`；因此CPU2从stage 10到scheduler ready的敏感窗口内不会
收到调度IPI。随后父线程固定CPU0，`mbox-v2`从首次指令起继承CPU0-only affinity，并直接
调用`bk7258_timer_start()`写CPU0私有SysTick。timer函数仍运行时检查
`up_cpu_index()==0`，但不再向CPU2发送任何IPI。反汇编已确认secondary stage检查位于
`sched_setaffinity`和`kthread_create`之前，worker中的timer调用也直接写SysTick寄存器。
heartbeat ACK同核发布和`heartbeat armed`诊断继续保留。

当前clean `-Werror`构建和标准打包产物：

```text
OpenVela AP / build/openvela-ap.bin / tmp/app1.bin SHA256
87ac12950fa5a22487c315efb75e0f3dfb40657ce1e1df2623561001ae7a096d

all-app.bin SHA256
1808720ced6480cf7dbdf175308036820877df21b24252749bf99d4ac1949dcd

app_ab_crc.rbl SHA256
02a6e6f8637606564d00b486f0791b41f70c6cd879724099646a10fb932d0354
```

2026-08-12 SMP IPI必达修复及启动回退检查点：

- 代码审计确认普通SysTick无法抢占本核正在执行的`hwspin_lock_irqsave()`：gate获取前
  `BASEPRI`已屏蔽普通中断，且Peterson gate通过`owner/depth`支持同CPU重入。因此
  “SysTick抢占同核atomic gate持有者并永久自锁”不是当前代码可成立的因果链。
- 实板CP在约2秒间隔内分别采到`detail[3]=0x001cc702`和`0x00248902`，tick计数增加
  `0x7c2=1986`；两次`detail[4]`均为`0xa8000000`，表示atomic gate完整释放。这直接
  排除CPU0长期关中断自旋和tick停摆。
- `bk7258_smp_kick()`原先在1024次FIFO重试后保留pending并返回，但只有CPU0 SysTick
  会补发CPU0到CPU1的pending；CPU1到CPU0的scheduler delivery或同步SMP call可能
  永久失去doorbell。当前普通SMP kick与RP2040契约一致，等待mailbox接受doorbell后
  才返回；非`EAGAIN`硬错误不再伪装为成功。SysTick ISR删除非标准
  `bk7258_smp_retry_pending()`，标准`nxsched_process_timer()`保持原位不动。
- PWC增加`primary=54`创建失败阶段和负errno detail，避免`kthread_create("pwc")`
  立即失败与停在创建过程都显示为`primary=48`。
- 修改前镜像`df6958cf...`可进入NSH、UART link ready、heartbeat ACK，并最终停在
  `primary=48, secondary=13`。上述IPI行为修改后的镜像出现启动回退：NSH可见，但
  UART link未ready，CP两次采样均停在`primary=31, secondary=13`，最终仍为
  `CPU1 boot timeout stage=4`。因此IPI必达缺陷真实存在，但尚不能视为当前启动失败
  的充分根因；最直接的回退相关行为差异是SMP kick从有界失败/后续tick补发改为发送者
  等待FIFO接受，以及tick ISR删除补发调用。

本检查点构建和烧录产物：

```text
OpenVela AP / build/openvela-ap.bin / tmp/app1.bin SHA256
d476f72daa453a72931406a1972d21d50a88a0a322659200f9a29bb5979327eb

all-app.bin SHA256
6f0bcb1e75efaa98311b58317455b6c2329452ec15468d0e5891dac11c6ae91c
```

独立NuttX工作树保持基线`dd92bcf4257`原始状态，不包含任何本地修改。后续诊断和修复
不得修改NuttX内核，范围限定在BK7258 board/SoC port与AP/CP集成层。

## 6. 协作约束

- 不提交 `projects/app_ab/build`、顶层 `build`或`__pycache__`。
- 不把构建生成的完整 AP/CP配置误当作最小项目配置同步回比赛仓；按第3节检查
  生成配置后恢复权威最小种子。
- SMP 工作树可能包含其他成员的未提交修改；同步覆盖前先检查`git diff`。
- 新增任何 SMP 源码修改时，在本目录保留对应路径的权威副本，并更新上面的
  覆盖表、构建步骤和验收项。
- 当前阶段已完成OpenVela AP allocator、四个媒体pool、空静态PSRAM section、CP lifecycle
  修复和离线重打包；尚未完成CP ID/容量query、NSH压力测试、DMA/media lease和实板验证。

## 7. UART0 Mailbox V2 使用和边界

CP 启动后默认 owner 为 `CP_CLI`。只有 MB_UART0 STATE 成功 ACK、IPC power-up 和
PWC boot-ready 三个条件同时成立，且`bk_mb_uart_write_ready()`非零时，
`ap_console open`才允许进入。heartbeat timeout、AP reset/power-off 和 Mailbox
reset/timeout都会清除门禁并请求回退到CP CLI。使用方式：

```text
ap_console status
ap_console open
<RAW AP NSH interaction>
<press Ctrl-], release it, then press .>
ap_console status
```

`open`在当前 CP CLI 响应经 UART TX-complete 后生效，并清 CP parser 和 UART RX；
RAW 模式不添加前缀。退出时按下`Ctrl-]`、松开，再按`.`；该转义不依赖行首状态。
链路 reset/timeout/down 时自动清理
未发送输入并回到 `CP_CLI`。`status`输出 owner/link/mode、bridge ring、MB_UART 状态、
active channel/sequence/command 和 drop/partial/overflow/reset/timeout 统计。

当前实现未声称以下项目已在实板验证：深睡 resume、AP/CP 独立重启、FIFO/stale ACK
故障注入和长时间双向压力。这些仍须按移植计划测试矩阵完成。

## 8. 2026-07-31 验证记录

已使用官方 `localhost/bekencorp/armino-idk:1.5` 镜像执行项目级 clean build。
验证结果：

```text
CP PSRAM_HEAP      0x60700000  0x00020000
AP PSRAM_HEAP      0x60720000  0x002e0000
AP PSRAM_SECTION   0x60a00000  0x00600000
all-app.bin        2646016 bytes
app_ab_crc.rbl     2576384 bytes
OpenVela AP SHA256 abd642f2a82842abb08c61a82c205afe5627b809fc47d328458d27bda44895c1
```

该记录只证明当时版本的`nuttx.bin`、`build/openvela-ap.bin`和
`package/tmp/app1.bin`一致。当前OpenVela AP已更新，当前hash及最终包状态以第5节
为准；重打包前不得继续引用本节旧hash作为当前修改已进入最终包的证据。
