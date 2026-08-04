# Beken AVDK 覆盖与构建基线

本目录保存 BK7258 OpenVela 最终固件构建所需的 Beken AVDK 覆盖文件，并作为
多人协作时 `bk_avdk_smp` 修改的同步记录。不要只修改工作区中的
`~/vela_competition/bk_avdk_smp`；可复现修改必须同时更新本目录及本文档。

## 1. 当前覆盖文件

| 比赛仓权威副本 | `bk_avdk_smp` 目标文件 | 作用 |
| --- | --- | --- |
| `cp/components/bk_cli/cli_main.c` | `cp/components/bk_cli/cli_main.c` | 注册 `ap_console` CLI 命令 |
| `cp/components/bk_cli/shell_task.c` | `cp/components/bk_cli/shell_task.c` | UART0 CP_CLI/AP_CONSOLE owner 状态机、延迟切换和退出转义 |
| `cp/include/components/ap_console_bridge.h` | `cp/include/components/ap_console_bridge.h` | CP UART0 bridge 模式、写入和统计接口 |
| `cp/include/components/shell_task.h` | `cp/include/components/shell_task.h` | shell 与 bridge 的切换/链路事件接口 |
| `cp/include/driver/mailbox_channel.h` | `cp/include/driver/mailbox_channel.h` | transport event、诊断和 poll 接口 |
| `cp/include/driver/mb_uart_driver.h` | `cp/include/driver/mb_uart_driver.h` | MB_UART link event、状态和 poll 接口 |
| `cp/middleware/driver/common/driver.c` | `cp/middleware/driver/common/driver.c` | MB_UART0 唯一 owner、LOG/RAW RX、1024 B TX ring 和 worker |
| `cp/middleware/driver/mailbox/mailbox_channel.c` | `cp/middleware/driver/mailbox/mailbox_channel.c` | 完整 ACK 匹配、timeout/reset、probe 和诊断 |
| `cp/middleware/driver/mailbox/mb_ipc_cmd.c` | `cp/middleware/driver/mailbox/mb_ipc_cmd.c` | IPC power-up ready 门禁事件 |
| `cp/middleware/driver/mailbox/mb_ipc_heartbeat.c` | `cp/middleware/driver/mailbox/mb_ipc_heartbeat.c` | heartbeat/reset/down 自动回退事件 |
| `cp/middleware/driver/mailbox/mb_uart_driver.c` | `cp/middleware/driver/mailbox/mb_uart_driver.c` | 严格 DATA/STATE 校验、ACK 后推进和 link event |
| `cp/middleware/driver/mailbox/mbox0_adapter.c` | `cp/middleware/driver/mailbox/mbox0_adapter.c` | command/ACK/reset/sync 分离稳定 slot |
| `cp/middleware/driver/mailbox/mbox0_adapter.h` | `cp/middleware/driver/mailbox/mbox0_adapter.h` | 稳定 slot 完成和 quarantine 接口 |
| `cp/middleware/driver/mailbox/mbox0_drv.c` | `cp/middleware/driver/mailbox/mbox0_drv.c` | RX 资源门禁、FIFO poll 和 destination count |
| `cp/middleware/driver/mailbox/mbox0_drv.h` | `cp/middleware/driver/mailbox/mbox0_drv.h` | MBOX0 poll、ready callback 和 FIFO count API |
| `cp/middleware/driver/pwr_clk/pwr_clk.c` | `cp/middleware/driver/pwr_clk/pwr_clk.c` | PWC boot-ready 门禁事件 |
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
  cp/components/bk_cli/cli_main.c \
  cp/components/bk_cli/shell_task.c \
  cp/include/components/ap_console_bridge.h \
  cp/include/components/shell_task.h \
  cp/include/driver/mailbox_channel.h \
  cp/include/driver/mb_uart_driver.h \
  cp/middleware/driver/common/driver.c \
  cp/middleware/driver/mailbox/mailbox_channel.c \
  cp/middleware/driver/mailbox/mb_ipc_cmd.c \
  cp/middleware/driver/mailbox/mb_ipc_heartbeat.c \
  cp/middleware/driver/mailbox/mb_uart_driver.c \
  cp/middleware/driver/mailbox/mbox0_adapter.c \
  cp/middleware/driver/mailbox/mbox0_adapter.h \
  cp/middleware/driver/mailbox/mbox0_drv.c \
  cp/middleware/driver/mailbox/mbox0_drv.h \
  cp/middleware/driver/pwr_clk/pwr_clk.c \
  projects/app_ab/partitions/bk7258/ram_regions.csv
do
  cp "$mirror/$path" "bk_avdk_smp/$path"
done
```

同步后必须逐字节检查：

```bash
for path in \
  cp/components/bk_cli/cli_main.c \
  cp/components/bk_cli/shell_task.c \
  cp/include/components/ap_console_bridge.h \
  cp/include/components/shell_task.h \
  cp/include/driver/mailbox_channel.h \
  cp/include/driver/mb_uart_driver.h \
  cp/middleware/driver/common/driver.c \
  cp/middleware/driver/mailbox/mailbox_channel.c \
  cp/middleware/driver/mailbox/mb_ipc_cmd.c \
  cp/middleware/driver/mailbox/mb_ipc_heartbeat.c \
  cp/middleware/driver/mailbox/mb_uart_driver.c \
  cp/middleware/driver/mailbox/mbox0_adapter.c \
  cp/middleware/driver/mailbox/mbox0_adapter.h \
  cp/middleware/driver/mailbox/mbox0_drv.c \
  cp/middleware/driver/mailbox/mbox0_drv.h \
  cp/middleware/driver/pwr_clk/pwr_clk.c \
  projects/app_ab/partitions/bk7258/ram_regions.csv
do
  cmp -s "$mirror/$path" "bk_avdk_smp/$path" || exit 1
done
```

全部`cmp`都必须返回0。

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
  localhost/bekencorp/armino-idk:1.5 \
  make -C projects/app_ab bk7258 \
  SDK_DIR=/armino \
  EXTERNAL_AP_BIN=/armino/build/openvela-ap.bin
```

`make -C projects/app_ab clean`只删除`projects/app_ab/build`，不会删除
`build/openvela-ap.bin`。仅安装 Podman 时不要使用内部调用 Docker 的`dbuild.sh`。

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
- 当前SMP协议兼容的`0x7/0x8/0x9/0xa`PWC处理和PSRAM_REQUIRED ready门禁。
- `0x60a00000..0x60ffffff`静态section链接保留；reset copy/clear未实现前强制0 B。

当前CP协议不通过`0xc`或其他命令暴露PSRAM initialized、device ID和actual capacity。
`0x7`响应只回显ON/OFF状态，不能单独证明PHY初始化成功或器件为16 MiB。因此当前
alias probe是临时运行时门禁，不能替代CP status协议扩展和整颗器件实板测试。

当前OpenVela构建和最终打包使用同一个 AP 输入：

```text
cmake_out/bk7258-ap_nsh/nuttx.bin
bk_avdk_smp/build/openvela-ap.bin
bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
SHA256 133cf4b8e54075bfac6ce70bfc1e042888ce22c1a64cc65742a7ee95653d8b6e
```

最终包已使用上述 OpenVela AP 重新生成：

```text
all-app.bin SHA256
93e540db51f262fd846620ca4257dfefbe5025f796466ab3fcf9fb6a29c51fe0

app_ab_crc.rbl SHA256
447f5eca7c0b3dafd6b47223aa1c7327478a9d9f9ca0db030104e5829c44a20c
```

除AP/CP `-Werror`编译、最终打包和AP输入一致性外，当前固件已在实板验证UART0
双向桥、IPC/PWC ready、heartbeat、PSRAM、NSH交互、CLE方向键与控制键、TTY
`Ctrl-C`信号以及AP控制台退出转义。

## 6. 协作约束

- 不提交 `projects/app_ab/build`、顶层 `build`或`__pycache__`。
- 不把构建生成的完整 AP/CP配置误当作最小项目配置同步回比赛仓。
- SMP 工作树可能包含其他成员的未提交修改；同步覆盖前先检查`git diff`。
- 新增任何 SMP 源码修改时，在本目录保留对应路径的权威副本，并更新上面的
  覆盖表、构建步骤和验收项。
- 当前阶段已完成OpenVela AP allocator、四个媒体pool和空静态PSRAM section的代码
  接入与离线构建；尚未完成CP ID/容量query、NSH压力测试、DMA/media lease、实板
  验证和最终包重打包。

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
