# 安装、更新与卸载

适用于 0.1.0 和 0.1.1 x64 Preview。推荐试用 0.1.1。

## 下载哪个文件

在仓库主页打开 **Releases**，进入对应版本，在 **Assets** 中下载 `SnipLeaf-版本号-x64-Preview-Setup.exe`。无需下载 GitHub 自动生成的 `Source code` 压缩包。

每个版本同时提供 `SHA256SUMS.txt` 和发布清单。可在下载目录用 PowerShell 检查：

```powershell
Get-FileHash -LiteralPath '.\SnipLeaf-0.1.1-x64-Preview-Setup.exe' -Algorithm SHA256
```

将结果与该版本 `SHA256SUMS.txt` 中的 EXE 哈希比对。哈希用于检查下载文件是否与发布附件一致，不能代替来源判断或公众信任的签名。

## 安装

1. 双击 EXE，阅读预览说明。当前没有公众信任的生产签名，Windows 或浏览器可能提示来源或信誉信息。
2. 安装向导需要明确同意信任 **SnipLeaf Preview** 专用公共证书。证书只进入“受信任人”库，不进入根证书库；安装包不包含证书私钥。必要时会出现 Windows 管理员确认。
3. 选择安装位置、桌面快捷方式及是否开机启动。默认位于 `%LOCALAPPDATA%\Programs\SnipLeaf`，开机启动默认关闭。
4. “补齐中文／英文 OCR”选项会在缺少组件时请求 Windows 安装对应可选功能，可能需要联网、管理员确认和重启。组件齐全后不会重复下载，识别可以离线运行。
5. 等待安装器检查应用身份和 OCR 语言。完成后从开始菜单启动 **SnipLeaf**；0.1.x 的入口可能仍显示“拾贴 SnipLeaf”。

普通使用者不需要安装 VS Code、C++ 编译器、Windows SDK、CMake 或 VC 运行库。日常截图与贴图使用普通用户权限。

只分发 EXE 而不安装所需身份与语言组件，不能保证 OCR 可用；请使用完整安装向导，安装后也不要手动移动程序目录。

## 从 0.1.0 更新到 0.1.1

这两个预览版需要先卸载再安装，直接覆盖会被安装器阻止。

1. 保存需要保留的贴图，右击托盘图标退出 SnipLeaf。
2. 在 Windows“已安装的应用”中卸载旧版。
3. 运行 0.1.1 安装包，按向导安装。

个人设置和 Windows OCR 语言组件保留。安装或卸载可能请求管理员确认，用于加入或清理本安装拥有的专用证书信任。

## 卸载

退出托盘程序后，在 Windows 设置中卸载，或使用开始菜单的“卸载 SnipLeaf”。卸载清理程序、快捷方式、应用身份和本安装新增的专用证书信任。

个人设置保留在 `%LOCALAPPDATA%\SnapLite\settings.ini`。该目录名称为历史兼容设置，软件对用户显示名称为 SnipLeaf。Windows OCR 语言组件也会保留。

## 验证范围

| 环境 | 状态 |
| --- | --- |
| 本机 Windows 11 x64、已有中英文 OCR | 安装、实际 OCR、功能回归验证通过 |
| 本机 0.1.0 卸载后安装 0.1.1 | 验证通过，个人设置保留 |
| 中文及空格安装路径 | 本机验证通过 |
| 干净 Windows 11、缺语言组件的首次安装 | 待独立实机验收 |
| Windows 10 | 安装器最低门槛为 2004；尚未完成实机验收 |
| ARM64 或 32 位 Windows | 暂无对应安装包 |

安装失败时可检查 `%TEMP%\SnipLeaf-install-last.log`，以及安装目录中的 `installation-check.json`。提交日志前请移除用户名、路径等个人信息。Windows Update 被组织策略限制时，可能需要管理员帮助安装 OCR 语言功能。
