# Git 与 Repo 操作说明

## 1. 只在参赛仓开发

当前仓库和分支：

```text
contest2026_264_VelaSightsuixingAIzhinengyanjing/
分支：dev-ai-contest-2026
```

BK7258 源码真实位置：

```text
board/beken/chips/bk7258/
board/beken/boards/bk7258/bk7258-ap/
```

只修改上述参赛仓目录。不要直接修改以下目录：

```text
vendor/beken/
nuttx/
bk_avdk_smp/
bk_idk/
```

这些目录只能用于读取参考代码。

## 2. Git 管理什么

参赛仓 Git 管理真实源码、静态库、配置和 manifest：

```text
board/beken/chips/bk7258/
board/beken/boards/bk7258/bk7258-ap/
contest2026_264_VelaSightsuixingAIzhinengyanjing.xml
git教程.md
```

不提交本地构建产物：

```text
cmake_out/
out/
nuttx
nuttx.bin
nuttx.map
.config
```

提交前检查：

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
git status --short --untracked-files=all
git diff --check
```

确认只包含预期文件后提交：

```bash
git add board/beken/chips/bk7258 \
        board/beken/boards/bk7258 \
        contest2026_264_VelaSightsuixingAIzhinengyanjing.xml \
        git教程.md
git commit -m "<简短说明修改内容>"
```

## 3. 软链接和 Manifest

构建树中的以下路径是参赛仓源码的入口，不是第二份源码：

```text
vendor/beken/chips/bk7258
vendor/beken/boards/bk7258/bk7258-ap
```

manifest 用 `linkfile` 创建入口：

```xml
<linkfile src="board/beken/chips/bk7258"
          dest="vendor/beken/chips/bk7258"/>
<linkfile src="board/beken/boards/bk7258"
          dest="vendor/beken/boards/bk7258"/>
```

Git 应提交真实源码和 manifest，不要把手动创建的软链接提交到 `vendor_beken` 仓库。

检查入口：

```bash
cd ~/vela_competition/contest
ls -l vendor/beken/chips/bk7258
ls -l vendor/beken/boards/bk7258/bk7258-ap
test -f vendor/beken/chips/bk7258/Kconfig && echo chip-ok
test -f vendor/beken/boards/bk7258/bk7258-ap/configs/nsh/defconfig && echo board-ok
```

本地提交不等于远端可同步。只有远端 manifest 引用包含上述 `linkfile` 的参赛仓 commit 后，全新工作区的 `repo sync` 才会自动创建入口。

同步：

```bash
cd ~/vela_competition/contest
repo sync -c -j8
```

查看 manifest 是否包含参赛仓和 Beken 仓：

```bash
repo manifest -r | grep -E 'contest2026_264|vendor/beken'
```

## 4. 构建 BK7258 AP

`lunch` 无参数菜单不会自动显示 BK7258，必须使用显式路径：

```bash
cd ~/vela_competition/contest
source build/envsetup.sh
lunch vendor/beken/boards/bk7258/bk7258-ap/configs/nsh
```

预期目标：

```text
[beken]-[bk7258-ap]-[nsh]
```

如果找不到交叉编译器：

```bash
export PATH=~/vela_competition/contest/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH
```

使用 CMake 构建：

```bash
./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  --cmake \
  -j8
```

或者使用已经 `lunch` 的环境：

```bash
m -j8
```

常见输出位置：

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

当前 `app_ab` 基线地址：

```text
AP Flash：0x02150000
AP RAM：  0x28010000..0x28064000
```

构建成功只表示 AP raw binary 已生成。最终 `all-app.bin` 仍需经过 BK7258 CP、bootloader 和标准 packager 打包。

## 5. 常见问题

### 菜单没有 BK7258

这是正常现象。使用：

```bash
lunch vendor/beken/boards/bk7258/bk7258-ap/configs/nsh
```

### 修改没有出现在 Git 状态

通常是修改了 `vendor/beken/...` 软链接入口。回到真实路径检查：

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
```

### `repo sync` 后入口消失

检查远端 manifest 是否已经引用包含 BK7258 `linkfile` 的 commit。仅在本地 `dev-ai-contest-2026` 分支提交，不能保证其他工作区自动同步。
