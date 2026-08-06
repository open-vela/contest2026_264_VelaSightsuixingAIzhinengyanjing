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
| `projects/app_ab/cp/cp_main.c` | `projects/app_ab/cp/cp_main.c` | OpenVela boot transaction 启动与失败 fail-stop |
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

权威 config 是 6 行最小种子（包含首个空行），完整生成配置只存在于 build 目录，
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

以下是 2026-08-06 本次 CP lifecycle 修改后的 OpenVela AP 打包输入：

```text
cmake_out/bk7258-ap_nsh/nuttx.bin
bk_avdk_smp/build/openvela-ap.bin
bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
SHA256 12706cd04af82b1934d629779995f5a0224cb7f88ce2b952c6c163cd228bfbae
```

本轮使用官方容器、项目级 clean 和 `EXTRA_CFLAGS=-Werror` 完整重建后的最终包：

```text
all-app.bin SHA256
d4a87345cd7a26a24788cd08d07910ef6fbb1d9a66e6cbe9b31899240c10c037

app_ab_crc.rbl SHA256
7ec2314944beb626f88cbbf714d78426e2cbf70e188bf188592e3d2647bfb89d
```

本轮已验证 CP `-Werror` 编译、最终打包、生成配置门禁、16 MiB 分区门禁和 AP 输入
逐字节一致性；尚未对新增 `0x11` commit/abort、recovery INIT 取消和 FINISH shutdown
执行实板验证。

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
