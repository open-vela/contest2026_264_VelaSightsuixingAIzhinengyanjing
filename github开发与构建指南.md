# Repo 多仓库开发与 Git 管理

## 1. 目录和职责

工作区：

```text
~/vela_competition/contest/
├── .repo/                                      # repo 元数据，不提交
├── nuttx/ apps/ packages/ vendor/              # openvela 公共仓库
└── contest2026_264_VelaSightsuixingAIzhinengyanjing/
                                                   # 比赛仓，唯一提交仓库
```

核心原则：

- `repo` 负责组织多个 Git 仓库；Git 负责版本管理。
- BK7258 源码只维护在比赛仓 `board/beken/` 下。
- `nuttx/`、`apps/`、`packages/` 和公共 `vendor/` 默认不修改、不提交。
- `vendor/beken/` 下的 BK7258 目录是 manifest 创建的构建入口，不是源码提交位置。

## 2. 初始化工作区

在新的工作区根目录执行，不要在比赛仓子目录中执行 `repo init`：

```bash
mkdir -p ~/vela_competition/contest
cd ~/vela_competition/contest

repo init \
  -u https://github.com/open-vela/contest2026_264_VelaSightsuixingAIzhinengyanjing \
  -b dev-ai-contest-2026 \
  -m contest2026_264_VelaSightsuixingAIzhinengyanjing.xml

repo sync -c -j8
repo status
```

比赛仓路径：

```text
~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/
```

## 3. 使用开发分支

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
git switch dev-ai-contest-2026
git branch --show-current
git status --short --branch
```

日常开发使用比赛仓的 `dev-ai-contest-2026` 或从该分支创建的功能分支。

## 4. 源码位置和 manifest 映射

当前 BK7258 映射为：

```xml
<linkfile src="board/beken/chips/bk7258"
          dest="vendor/beken/chips/bk7258"/>
<linkfile src="board/beken/boards/bk7258"
          dest="vendor/beken/boards/bk7258"/>
```

对应关系：

```text
比赛仓/board/beken/chips/bk7258/
    -> 工作区/vendor/beken/chips/bk7258/

比赛仓/board/beken/boards/bk7258/
    -> 工作区/vendor/beken/boards/bk7258/
```

源码应放在：

```text
contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
```

不要直接把代码放到：

```text
contest/vendor/beken/
contest/nuttx/
contest/apps/
contest/packages/
```

如果新增源码目录，必须同时在比赛仓 manifest 中增加对应的 `<linkfile>`，然后同步并检查映射：

```bash
cd ~/vela_competition/contest
repo sync -c
ls -ld vendor/beken/chips/bk7258
readlink vendor/beken/chips/bk7258
```

## 5. 日常开发和构建

编辑比赛仓中的源码：

```text
board/beken/chips/bk7258/
board/beken/boards/bk7258/bk7258-ap/
```

BK7258 显式构建：

```bash
cd ~/vela_competition/contest
source build/envsetup.sh
lunch vendor/beken/boards/bk7258/bk7258-ap/configs/nsh
m -j8
```

使用 CMake：

```bash
./build.sh \
  vendor/beken/boards/bk7258/bk7258-ap/configs/nsh \
  --cmake \
  -j8
```

`lunch` 菜单只显示已注册的公共 vendorsetup 组合，不代表 BK7258 不存在。BK7258 使用上面的显式路径。

## 6. 正确提交

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing

git status --short
git diff --check

git add board/beken
git add contest2026_264_VelaSightsuixingAIzhinengyanjing.xml
git add git教程.md

git diff --cached --name-only
git commit -m "feat: complete BK7258 porting milestone"

git status --short --branch
git log --oneline -5
```

暂存内容只能来自比赛仓。提交中不应出现：

```text
nuttx/
apps/
packages/
vendor/
.repo/
cmake_out/
out/
```

提交到远端：

```bash
git push origin dev-ai-contest-2026
```

## 7. 功能分支

```bash
git switch dev-ai-contest-2026
git switch -c feature/bk7258-startup

git status
git diff --check
git diff dev-ai-contest-2026...HEAD --stat
```

完成后合并回 `dev-ai-contest-2026`：

```bash
git switch dev-ai-contest-2026
git merge --no-ff feature/bk7258-startup
```

不建议执行：

```bash
repo start dev-ai-contest-2026 --all
```

它会给所有公共仓库创建额外分支。

## 8. 检查公共仓库

```bash
cd ~/vela_competition/contest
repo status
git -C nuttx status
git -C vendor/beken status
git -C apps status
```

公共仓库出现修改时：

- BK7258 移植代码放回比赛仓 `board/beken/`。
- openvela 公共功能在对应公共仓库单独建分支、提交和发 PR。
- 不要复制公共仓库文件到比赛仓来掩盖公共仓修改。

## 9. 同步上游

同步前先检查：

```bash
cd ~/vela_competition/contest
repo status
```

同步：

```bash
repo sync -c -j8
```

不要在未确认本地修改可丢弃时执行：

```bash
git reset --hard
git checkout -- .
repo sync --force-sync
```

## 10. 标签和日志

阶段完成后可在比赛仓创建标签：

```bash
git tag -a bk7258-startup -m "BK7258 startup baseline"
git tag
```

AI Coding 日志放在：

```text
contest2026_264_VelaSightsuixingAIzhinengyanjing/logs/
```

不要把日志放到 `.repo/`，也不要提交构建临时日志。

## 11. 最终原则

- 比赛仓是参赛代码的唯一提交仓库。
- Git 管理比赛仓源码和 manifest；manifest 管理工作区映射。
- 软链接或 linkfile 只是构建入口，不是另一份源码。
- 每次提交前检查 `git diff --cached --name-only`。
- 每个阶段完成后检查 `repo status`。
- 公共仓库需要修改时，单独管理、单独提交、单独发 PR。
