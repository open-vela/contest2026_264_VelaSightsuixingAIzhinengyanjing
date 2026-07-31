# Beken AVDK 覆盖与构建基线

本目录保存 BK7258 OpenVela 最终固件构建所需的 Beken AVDK 覆盖文件，并作为
多人协作时 `bk_avdk_smp` 修改的同步记录。不要只修改工作区中的
`~/vela_competition/bk_avdk_smp`；可复现修改必须同时更新本目录及本文档。

## 1. 当前覆盖文件

| 比赛仓权威副本 | `bk_avdk_smp` 目标文件 | 作用 |
| --- | --- | --- |
| `cp/middleware/driver/common/driver.c` | `cp/middleware/driver/common/driver.c` | CP 启动时初始化 PSRAM，并提供 AP 日志转发 |
| `projects/app_ab/partitions/bk7258/ram_regions.csv` | `projects/app_ab/partitions/bk7258/ram_regions.csv` | BK7258 640 KiB Share SRAM 和 16 MiB PSRAM 布局 |

`driver.c`还包含 CP shell 队列串行化、AP 日志按行缓存、`ap0:`来源前缀、
50 ms 半行刷新和队列提交失败有限重试。

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
cp \
  contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/bk_avdk_smp/cp/middleware/driver/common/driver.c \
  bk_avdk_smp/cp/middleware/driver/common/driver.c

cp \
  contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv \
  bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv
```

同步后必须逐字节检查：

```bash
cmp -s \
  contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/bk_avdk_smp/cp/middleware/driver/common/driver.c \
  bk_avdk_smp/cp/middleware/driver/common/driver.c

cmp -s \
  contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/external/bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv \
  bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv
```

两个`cmp`都必须返回0。

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

当前OpenVela构建产物为：

```text
cmake_out/bk7258-ap_nsh/nuttx.bin
SHA256 185b775f22c4278791b2e1c3bb811ea157df5bfde9085cc7a15824b37ca0a64f
```

本轮已重新执行`app_ab`项目级clean build。以下三个AP文件完全一致：

```text
cmake_out/bk7258-ap_nsh/nuttx.bin
bk_avdk_smp/build/openvela-ap.bin
bk_avdk_smp/projects/app_ab/build/bk7258/app_ab/package/tmp/app1.bin
SHA256 185b775f22c4278791b2e1c3bb811ea157df5bfde9085cc7a15824b37ca0a64f
```

最终包也已重新生成：

```text
all-app.bin SHA256
cf576afe765743fece43602ca7dba4e1ed496732d77767fed27aed95e0dfbd04

app_ab_crc.rbl SHA256
f89e6f0ad878d3d0908d60f57e0cba6ff65fb662f5f238acf3685db36e572f30
```

本版只修改OpenVela AP，未修改任何CP源码：`git diff --exit-code`对
`cp/middleware/driver/pwr_clk/pwr_clk.c`和`low_pwr_core.c`均为空。AP侧修复了
接收非PWC/UART逻辑通道时不回transport ACK导致CP物理通道永久BUSY、进而阻塞
后续PWC语义响应的问题，与原厂mailbox channel层“无handler仍回COM_FAIL ACK”
的行为一致。

因此本轮OpenVela PSRAM allocator代码已经进入当前最终可烧录包。这里证明的是
构建和打包输入一致，不等同于实板PSRAM ID、容量、地址线、PWC和allocator压力
验收已经通过。

## 6. 协作约束

- 不提交 `projects/app_ab/build`、顶层 `build`或`__pycache__`。
- 不把构建生成的完整 AP/CP配置误当作最小项目配置同步回比赛仓。
- SMP 工作树可能包含其他成员的未提交修改；同步覆盖前先检查`git diff`。
- 新增任何 SMP 源码修改时，在本目录保留对应路径的权威副本，并更新上面的
  覆盖表、构建步骤和验收项。
- 当前阶段已完成OpenVela AP allocator、四个媒体pool和空静态PSRAM section的代码
  接入与离线构建；尚未完成CP ID/容量query、NSH压力测试、DMA/media lease、实板
  验证和最终包重打包。

## 7. 2026-07-31 验证记录

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
