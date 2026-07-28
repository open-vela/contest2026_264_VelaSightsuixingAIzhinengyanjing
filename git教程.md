# 工作目录说明

本项目是 BK7258 openvela AP 移植工程。所有移植源码、配置、静态库和项目文档只在参赛仓中开发，并通过 manifest 暴露到 openvela 编译树。

| 工程 | 作用 | Git 管理方式 |
| --- | --- | --- |
| `contest2026_264_VelaSightsuixingAIzhinengyanjing/` | 参赛仓，保存 BK7258 移植代码 | 本项目主要 Git 仓库 |
| `vendor/beken/` | openvela 编译树中的 Beken 工程入口 | 独立 `vendor_beken` project，不直接写入 BK7258 源码 |
| `nuttx/` | openvela/NuttX 公共源码 | 只读参考，除非另有公共仓库提交要求 |
| `bk_avdk_smp/` | BK7258 CP、bootloader 和打包工程 | 只读参考 |
| `bk_idk/` | BK7258 寄存器和底层驱动参考 | 只读参考 |

正式开发分支：

```text
dev-ai-contest-2026
```

> 本地分支必须与远端 manifest 使用的开发分支保持一致。提交前先用 `git branch --show-current` 核对分支。

## BK7258 源码目录和原则

参赛仓中的真实源码目录：

```text
contest2026_264_VelaSightsuixingAIzhinengyanjing/
├── board/beken/chips/bk7258/
└── board/beken/boards/bk7258/bk7258-ap/
```

开发原则：

- 只修改参赛仓中的真实路径。
- 不把 BK7258 源码直接提交到 `vendor/beken/`。
- 不把 `vendor/beken/...` 的软链接当作第二份源码修改。
- 不提交本地构建目录、ELF、BIN、MAP 和 `.config`。
- 每次完成一组修改后执行检查并提交。

# Git 管理方案

## 1. Git 管理范围

参赛仓 Git 管理以下内容：

```text
board/beken/chips/bk7258/
board/beken/boards/bk7258/bk7258-ap/
contest2026_264_VelaSightsuixingAIzhinengyanjing.xml
```

其中包括：

- BK7258 C/C++ 源码、头文件和汇编文件
- `Kconfig`、`CMakeLists.txt` 和 `Make.defs`
- NSH `defconfig`
- BK7258 链接脚本
- 随项目发布的静态库
- repo manifest 和项目文档

以下内容属于本地构建产物，不提交：

```text
cmake_out/
out/
nuttx
nuttx.bin
nuttx.map
.config
```

## 2. 提交前检查

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing

git branch --show-current
git status --short --untracked-files=all
git diff --check
git diff --stat
```

必须确认：

- 当前分支为 `dev-ai-contest-2026`。
- 修改只位于参赛仓预期目录。
- 没有构建产物或无关文件。
- `git diff --check` 没有输出。

提交：

```bash
git add board/beken/chips/bk7258 \
        board/beken/boards/bk7258 \
        contest2026_264_VelaSightsuixingAIzhinengyanjing.xml \
        git教程.md
git commit -m "<简短说明修改内容>"
```

检查提交：

```bash
git show --stat --oneline HEAD
git show --check HEAD
git status --short
```

# Manifest 和软链接

## 1. 目录关系

`vendor/beken/chips/bk7258` 和 `vendor/beken/boards/bk7258/bk7258-ap` 是编译树入口，不是源码副本：

```text
参赛仓真实源码
  contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
        │
        └── manifest linkfile
              │
              ▼
openvela 编译入口
  vendor/beken/chips/bk7258
  vendor/beken/boards/bk7258/bk7258-ap
```

软链接本身可以由 Git 管理，但 Git 只保存链接目标字符串，不保存目标目录内容。本项目不把手动创建的入口提交到 `vendor_beken` 仓库，而是提交参赛仓源码和 manifest 配置。

## 2. Manifest 配置

参赛仓 manifest 使用：

```xml
<linkfile src="board/beken/chips/bk7258"
          dest="vendor/beken/chips/bk7258"/>
<linkfile src="board/beken/boards/bk7258"
          dest="vendor/beken/boards/bk7258"/>
```

不得使用以下方式覆盖整个 Beken project：

```xml
<linkfile src="board/beken" dest="vendor/beken"/>
```

原因是 `vendor/beken` 已由基础 manifest 声明为独立的 `vendor_beken` project，整目录覆盖会产生 project/linkfile 冲突。

## 3. 入口检查

```bash
cd ~/vela_competition/contest

ls -l vendor/beken/chips/bk7258
ls -l vendor/beken/boards/bk7258/bk7258-ap

test -f vendor/beken/chips/bk7258/Kconfig && echo chip-ok
test -f vendor/beken/boards/bk7258/bk7258-ap/configs/nsh/defconfig && echo board-ok
```

查看同步后的 manifest：

```bash
repo manifest -r | grep -E 'contest2026_264|vendor/beken'
```

## 4. Repo 同步

同步完整工作区：

```bash
cd ~/vela_competition/contest
repo sync -c -j8
```

只进行同步检查，不下载文件：

```bash
repo sync -n --no-tags contest2026_264_VelaSightsuixingAIzhinengyanjing
```

同步成立的必要条件：

| 条件 | 要求 |
| --- | --- |
| 参赛仓 commit | 已提交并推送到远端可访问分支 |
| manifest | 引用包含 BK7258 `linkfile` 的版本 |
| 分支 | 使用 `dev-ai-contest-2026` |
| 目录结构 | 参赛仓路径与 manifest `src` 一致 |

本地 commit 不等于远端可同步。只有远端 manifest 引用了包含最新配置的参赛仓 commit，全新工作区执行 `repo sync` 后才会自动创建 BK7258 入口。

# BK7258 AP 构建

## 1. 显式选择目标

无参数 `lunch` 菜单只显示通过 `vendorsetup.sh` 注册的组合，不会自动列出本参赛仓 BK7258 目标。使用显式路径：

```bash
cd ~/vela_competition/contest
source build/envsetup.sh
lunch vendor/beken/boards/bk7258/bk7258-ap/configs/nsh
```

预期目标：

```text
[beken]-[bk7258-ap]-[nsh]
```

## 2. 配置工具链

如果找不到 `arm-none-eabi-gcc`：

```bash
export PATH=~/vela_competition/contest/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH
```

检查：

```bash
arm-none-eabi-gcc --version
```

## 3. 构建命令

使用已选择的目标构建：

```bash
m -j8
```

直接使用统一入口进行 CMake 构建：

```bash
./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  --cmake \
  -j8
```

也可以指定 CMake 输出目录：

```bash
./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  --cmake \
  -b cmake_out/bk7258-ap_nsh \
  -j8
```

典型输出：

```text
out/beken_bk7258-ap_nsh/
cmake_out/bk7258-ap_nsh/
```

典型产物：

```text
nuttx
nuttx.bin
nuttx.map
```

构建成功只表示 OpenVela AP raw binary 已生成，不代表最终 `all-app.bin` 已完成。最终固件还需要经过 BK7258 CP、bootloader、分区配置和标准 packager。

# 验证和问题定位

## 1. Git 状态验证

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
git status --short --untracked-files=all
```

空输出表示当前仓库工作区干净。

## 2. ELF 验证

```bash
cd ~/vela_competition/contest
arm-none-eabi-readelf -h cmake_out/bk7258-ap_nsh/nuttx
```

重点确认：

- `Class: ELF32`
- `Machine: ARM`
- `hard-float ABI`
- 入口位于 AP Flash 区域

检查关键符号：

```bash
arm-none-eabi-readelf -s cmake_out/bk7258-ap_nsh/nuttx | \
  grep -E '__start|_vectors|__ram_vectors|__heap_|__idle_stack'
```

当前 `app_ab` 基线：

```text
AP Flash：0x02150000
AP RAM：  0x28010000..0x28064000
```

## 3. 常见问题

### `lunch` 菜单没有 BK7258

这是正常现象。直接使用：

```bash
lunch vendor/beken/boards/bk7258/bk7258-ap/configs/nsh
```

### 修改没有出现在 Git 状态

通常是修改了 `vendor/beken/...` 软链接入口。切换到参赛仓真实路径：

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
```

### `repo sync` 后 BK7258 入口消失

依次检查：

```bash
git branch --show-current
repo manifest -r | grep -E 'contest2026_264|vendor/beken'
ls -l ~/vela_competition/contest/vendor/beken/chips/bk7258
```

确认当前使用的是 `dev-ai-contest-2026`，并且远端 manifest 已引用包含 BK7258 linkfile 的 commit。
