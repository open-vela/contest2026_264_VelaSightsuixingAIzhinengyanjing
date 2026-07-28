# Git 与 Repo 开发教程

本文说明 BK7258 openvela 移植项目的 Git 管理、manifest 同步和构建方式。

## 1. 开发位置

所有 BK7258 移植源码只在参赛仓目录内开发：

```text
contest2026_264_VelaSightsuixingAIzhinengyanjing/
```

当前开发分支为：

```text
dev
```

BK7258 主要源码目录：

```text
board/beken/chips/bk7258/
board/beken/boards/bk7258/bk7258-ap/
```

不要直接在以下目录修改 BK7258 源码：

```text
vendor/beken/
nuttx/
bk_avdk_smp/
bk_idk/
```

这些目录属于 openvela、Beken SDK 或其他独立工程。参考代码可以读取，但移植修改应回写到参赛仓目录。

## 2. Git 管理范围

Git 管理的是参赛仓中的真实源码和 manifest 文件：

```text
board/beken/chips/bk7258/
board/beken/boards/bk7258/bk7258-ap/
contest2026_264_VelaSightsuixingAIzhinengyanjing.xml
git教程.md
```

其中包括：

- C、C++、头文件和汇编文件
- `Kconfig`
- `CMakeLists.txt`
- `Make.defs`
- NSH `defconfig`
- BK7258 链接脚本
- 需要随项目发布的静态库
- repo manifest
- 项目说明和开发教程

查看当前仓库状态：

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
git status --short
git branch --show-current
git log --oneline -5
```

正常情况下，`git status --short` 没有输出表示工作区干净。

## 3. 软链接与 Manifest

软链接是指向另一个路径的文件系统入口，不是源码副本。例如工作区中可能存在：

```text
vendor/beken/chips/bk7258
vendor/beken/boards/bk7258/bk7258-ap
```

它们分别指向参赛仓中的：

```text
contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/chips/bk7258
contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/boards/bk7258/bk7258-ap
```

源码只有一份，真实位置在参赛仓中。

当前 manifest 使用 `linkfile` 将参赛仓目录暴露到 openvela 编译树：

```xml
<linkfile src="board/beken/chips/bk7258"
          dest="vendor/beken/chips/bk7258"/>
<linkfile src="board/beken/boards/bk7258"
          dest="vendor/beken/boards/bk7258"/>
```

Git 可以管理软链接本身，但 Git 保存的只是链接目标字符串，不会把目标目录再次复制进去。对于本项目，应该提交：

- 参赛仓中的真实源码
- manifest 中的 `linkfile` 配置

不应该把手动创建的 `vendor/beken` 软链接作为 `vendor_beken` 仓库中的源码提交。

检查软链接：

```bash
cd ~/vela_competition/contest
ls -l vendor/beken/chips/bk7258
ls -l vendor/beken/boards/bk7258/bk7258-ap
```

检查源码是否可访问：

```bash
test -f vendor/beken/chips/bk7258/Kconfig && echo chip-ok
test -f vendor/beken/boards/bk7258/bk7258-ap/configs/nsh/defconfig && echo board-ok
```

软链接使用相对路径，换工作区或换机器时仍要求 manifest、参赛仓 commit 和目录结构保持一致。

## 4. 提交修改

每次修改后先检查：

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
git status --short
git diff --check
git diff --stat
```

确认只包含预期文件后提交：

```bash
git add board/beken/chips/bk7258 \
        board/beken/boards/bk7258 \
        contest2026_264_VelaSightsuixingAIzhinengyanjing.xml \
        git教程.md
git commit -m "<简短说明修改内容>"
```

查看提交内容：

```bash
git show --stat --oneline HEAD
git show --check HEAD
```

不要提交以下本地构建产物：

```text
cmake_out/
out/
nuttx
nuttx.bin
nuttx.map
.config
```

提交前确认未跟踪文件：

```bash
git status --short --untracked-files=all
```

## 5. Repo 同步

查看当前 manifest 中的参赛仓和 Beken 仓：

```bash
cd ~/vela_competition/contest
repo list | grep -E 'contest2026_264|vendor/beken'
```

执行同步：

```bash
repo sync -c -j8
```

只检查同步过程、不实际下载文件：

```bash
repo sync -n --no-tags contest2026_264_VelaSightsuixingAIzhinengyanjing
```

同步后检查 BK7258 入口：

```bash
ls -l vendor/beken/chips/bk7258
ls -l vendor/beken/boards/bk7258/bk7258-ap
```

注意：`repo sync` 是否能在全新工作区自动创建 BK7258 软链接，取决于远端 manifest 所引用的参赛仓 commit 是否已经包含最新 manifest 修改。仅本地提交不等于远端同步已经完成。

## 6. 选择构建目标

`lunch` 的无参数菜单只显示通过 `vendorsetup.sh` 注册的组合，因此菜单中不一定出现 BK7258。当前 BK7258 应使用显式路径：

```bash
cd ~/vela_competition/contest
source build/envsetup.sh
lunch vendor/beken/boards/bk7258/bk7258-ap/configs/nsh
```

预期目标：

```text
[beken]-[bk7258-ap]-[nsh]
```

也可以不进入交互菜单，直接使用统一构建入口：

```bash
./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  --cmake \
  -j8
```

## 7. 构建 BK7258 AP

如果当前 shell 没有找到交叉编译器，先设置工具链：

```bash
export PATH=~/vela_competition/contest/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH
```

使用 CMake 构建：

```bash
cd ~/vela_competition/contest
source build/envsetup.sh
lunch vendor/beken/boards/bk7258/bk7258-ap/configs/nsh
m -j8
```

构建输出默认位于：

```text
out/beken_bk7258-ap_nsh/
```

也可以指定单独的 CMake 输出目录：

```bash
./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  --cmake \
  -b cmake_out/bk7258-ap_nsh \
  -j8
```

常见产物：

```text
cmake_out/bk7258-ap_nsh/nuttx
cmake_out/bk7258-ap_nsh/nuttx.bin
cmake_out/bk7258-ap_nsh/nuttx.map
```

## 8. 构建检查

检查 ELF 架构和 ABI：

```bash
arm-none-eabi-readelf -h cmake_out/bk7258-ap_nsh/nuttx
```

应重点确认：

- `Machine: ARM`
- `ELF32`
- `hard-float ABI`
- 入口位于 AP Flash 区域

检查关键符号和内存范围：

```bash
arm-none-eabi-readelf -s cmake_out/bk7258-ap_nsh/nuttx | \
  grep -E '__start|_vectors|__ram_vectors|__heap_|__idle_stack'
```

当前 `app_ab` 基线的关键地址：

```text
AP Flash：0x02150000
AP RAM：  0x28010000..0x28064000
```

构建成功不等于已完成最终固件打包。OpenVela AP raw binary 还需要通过 BK7258 的 CP、bootloader 和标准 packager 接入最终 `all-app.bin`。

## 9. 常见问题

### `lunch` 菜单没有 BK7258

使用显式路径：

```bash
lunch vendor/beken/boards/bk7258/bk7258-ap/configs/nsh
```

### 找不到 `arm-none-eabi-gcc`

设置工具链 PATH：

```bash
export PATH=~/vela_competition/contest/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH
```

### CMake 使用了不存在的 `ccache`

如果系统没有安装 `ccache`，直接使用 `cmake`，或清理 CMake 输出目录后重新配置。不要把 `ccache` 路径写入项目源码。

### 修改没有出现在 Git 状态中

确认修改的是参赛仓真实路径，而不是构建树中的软链接入口：

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
git status --short
```

### `repo sync` 后 BK7258 入口消失

检查以下内容：

```bash
repo manifest -r | grep -E 'contest2026_264|vendor/beken'
git -C contest2026_264_VelaSightsuixingAIzhinengyanjing log --oneline -5
```

如果远端 manifest 没有引用包含 BK7258 linkfile 的 commit，需要先将参赛仓对应分支推送到允许同步的远端分支，再重新执行 `repo sync`。
