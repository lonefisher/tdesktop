# Fishgram Windows 构建机

`fishgram-checks.yml` 里的依赖预热（warmup）和测试身份编译（compile）按下述规则选 runner。`lonefisher/fishgram` 的 product candidate 工作流使用同一条表达式、同一组标签。

```yaml
runs-on: ${{ fromJSON(((github.event_name == 'pull_request' && github.event.pull_request.head.repo.full_name != github.repository) || github.event.inputs.use_github_hosted == 'true') && '["windows-2022"]' || '["self-hosted", "Windows", "X64", "fishgram-builder"]') }}
```

- 推送到本仓库分支（含 `custom/main`）、从本仓库分支打开的 pull request、以及手动运行：排队到同时带有 `self-hosted`、`Windows`、`X64`、`fishgram-builder` 的 runner。构建机关机就继续排队，不探测是否在线，也不使用 token。
- Fork 的 pull request 固定走 `windows-2022`。判断方式是 head 仓库全名不等于本仓库，而不是 `head.repo.fork`。本仓库本身是上游的 fork，`fork == true` 会把本仓库自己的 pull request 也送去托管 runner。Fork 触发的运行使用基线分支上的工作流文件，改不了 `runs-on`。
- 手动运行有布尔输入 `use_github_hosted`，默认 false。勾上之后这次运行用 `windows-2022`。
- `Production helper tests` 始终在 `windows-2022`。它会把另一份 fishgram 检出到 `product/`，不能和构建机上留下来的依赖、构建目录放在一起。
- GitHub 会把排队超过 24 小时的 job 判为失败。构建机重新开机后，重新跑那次工作流即可。

## 注册本仓库的 runner

个人账号只能注册仓库级 runner。这台 PC 上每个仓库单独一个，目录分开，例如 fishgram 用 `C:\fishgram-ci\fishgram`，本仓库用下面的路径。

1. 安装 [PowerShell 7](https://github.com/PowerShell/PowerShell/releases) 的机器级版本，让系统 PATH 里有 `pwsh`。依赖准备会直接调用 `pwsh`（包括解压 jom）。Windows PowerShell 5.1 不够。Runner 服务账户也必须能执行到这个 `pwsh`。
2. 这台机器上已经具备、并且构建会继续使用的工具：
   - Visual Studio 2022 Build Tools 17.14，`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`
   - MSVC 14.44.35207（`VC\Tools\MSVC\14.44.35207`）
   - Windows SDK 10.0.26100.0
   - Git。安装器若只改了当前用户的 PATH，以服务方式运行的 runner 看不到 `git`。这台机器上的 `pwsh` 在 `C:\Program Files (x86)\PowerShell\7`，是 32 位进程，它的 `ProgramFiles` 不是 64 位的 `C:\Program Files`。作业会再查 `ProgramW6432` 和 `C:\Program Files\Git\cmd`。装在别的位置时，把 `Git\cmd` 加到系统 PATH，然后重启 runner 服务
   - CMake 和 Ninja：随 Visual Studio 的 “C++ CMake tools for Windows” 组件安装，或另外放到 PATH
3. 打开本仓库 Settings → Actions → Runners → New self-hosted runner，选择 Windows、x64。把压缩包解压到 `C:\fishgram-ci\tdesktop`。
4. 在该目录执行。Token 用页面上显示的那一个，它用过即失效：

```bat
config.cmd --url https://github.com/lonefisher/tdesktop --token <页面上的 token> --name tdesktop --labels fishgram-builder --work C:\fishgram-ci\tdesktop-work --unattended --runasservice
```

默认标签 `self-hosted`、`Windows`、`X64` 会自动加上，只要再加 `fishgram-builder`。不要选 ephemeral。这个仓库只留这一个 runner 进程，两个作业共用 `C:\fishgram-ci\tdesktop-work`。

5. 不要删除、不要定期清理 `C:\fishgram-ci\tdesktop-work`。自托管作业的 checkout 使用 `clean: false`，`Libraries`、`ThirdParty` 和 `build-modified` 会留在这棵目录里。第一次运行会完整准备依赖并完整编译，之后只重编有变化的部分。作业会执行 `git config --global core.longpaths true`。
6. 装好后在 Runners 页面应看到 Idle，四个标签都在。C: 上大约 1.9 TB 空闲，依赖和构建树几十 GB，放得下。

机器上的 Python 3.15 可以留着。工作流自己用 `actions/setup-python` 安装 3.13。

## MSVC 14.44

`docs/building-win.md` 要求 x64 使用 `-vcvars_ver=14.44`（v144.4）。fishgram 配方里的 `msvc` 也是 `14.44`，但 `Get-FishGramToolchain` 默认用 `vswhere -latest` 取最新的 Visual Studio。以后如果再装上 Visual Studio 2026，而 14.44 仍只在 2022 Build Tools 里，最新安装里就找不到这个工具集。

自托管作业会在所有安装（含预发布）里查找 `VC\Tools\MSVC\14.44.*` 和 `vcvars64.bat`，把含有最高 14.44 版本的安装路径写进工作目录中的 `tools.local.json`（不提交）。随后仍由配方执行 `vcvars64.bat 10.0.26100.0 -vcvars_ver=14.44`。选中的是 14.44 工具集，不是 VS 2026 默认的 v145。`cmake`、`ninja`、`python` 这三项已有的本地覆盖会保留。

## GitHub-hosted 路径

跑在 `windows-2022` 上时，缓存行为和现在相同：同一套缓存键、压缩包不超过 8 GB、只有 `custom/main` 的 push 或手动运行可以写入、pull request 只读。不加大缓存，也不加 keepalive。

构建机上的作业不读、不写这份缓存。缓存键包含托管镜像的 `ImageVersion` 和 `ImageOS`，构建机没有这两项。构建机关机时，`custom/main` 的预热会在队列里等待，托管缓存就停在上一次由托管 runner 写好的内容。Fork 或勾选 `use_github_hosted` 时如果没命中，会像现在缓存未命中一样，在托管 runner 上重新准备依赖。要刷新托管缓存，在 `custom/main` 上手动运行并勾选 `use_github_hosted`。

## 限制

- 没有“构建机离线就改走托管 runner”。关机期间符合条件的 warmup / compile 会排队，超过约 24 小时后 GitHub 把这次运行判失败，重新跑即可。
- 本仓库里的 pull request 会在构建机上编译该 PR 的代码和工作流。只把信任的分支推到这个仓库。Fork 不会进构建机。
- 运行被取消时，`build-modified` 可能停在半截。下次会重新配置。增量状态坏了就只删工作目录里的 `build-modified`，留下 `Libraries` 和 `ThirdParty`。
- 编译开始前会删掉上次留在 `reports` 里的 `*-diagnostics.json`，上传的诊断包只含当次运行。
