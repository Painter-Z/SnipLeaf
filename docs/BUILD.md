# 从源码构建

## 工具

- Windows x64，当前验证环境为 Windows 11。
- Visual Studio 2022 或 Build Tools 2022，安装“使用 C++ 的桌面开发”与 x64 MSVC。根目录 `toolchain.vsconfig` 可用于选择组件。
- Windows SDK；当前使用 10.0.26100，需含 C++/WinRT 头文件、MakeAppx 和 SignTool。
- CMake 3.28 或更新版本，加入 PATH。
- Windows PowerShell 5.1；应用身份和证书相关脚本按此环境验证。
- 生成 EXE 安装器时另需 Inno Setup 7.1 或更新版本。请核对所用编译器版本的许可。

VS Code 的 C++ 扩展提供编辑功能，编译仍需要上述工具链。源码不捆绑编译器、SDK 或系统 OCR 模型。

## 编译

在仓库根目录的 Windows PowerShell 5.1 中执行：

```powershell
.\scripts\Build.ps1
```

脚本通过 Visual Studio 安装信息检测 MSVC，SDK 工具通过环境与注册表检测，不依赖原作者的磁盘路径。输出为 `build\Release\SnapLite.exe`。`SnapLite` 是沿用的内部构建目标和开发身份名，软件界面名称为 SnipLeaf。

也可以在 x64 Visual Studio 开发终端中直接运行：

```powershell
cmake -S . -B build\manual -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build\manual
```

## 本机开发 OCR

Windows OCR 桌面调用需要应用身份和系统识别语言。开发脚本使用独立的 `SnapLite.Development` 身份，绑定 `build\Release`：

```powershell
.\scripts\Register-DevPackage.ps1 -TrustMachine
.\build\Release\SnapLite.exe
```

该步骤会生成专用自签名开发证书、签名小型身份包，Windows 会请求管理员确认，将公共证书加入“受信任人”，不加入根证书库；签名私钥在签名后删除。它是显式的本机开发信任流程，不会获得公众发行信任，也不会解决智能应用控制拦截。

同一台电脑若已有开发身份绑定其他源码目录，脚本会拒绝覆盖。需要切换目录时，先退出原开发程序，在原目录运行 `Unregister-DevPackage.ps1` 完成清理，再注册新目录。正常运行开发版前请退出安装版，两者共用单实例保护和默认热键。

中文／英文 OCR 组件可通过软件托盘设置中的“配置 OCR 语言…”准备。首次补齐可能需要联网、管理员确认及 Windows 要求的重启。

退出并清理本目录开发身份：

```powershell
.\build\Release\SnapLite.exe --quit
.\scripts\Unregister-DevPackage.ps1
```

## 检查

完成应用身份和 OCR 语言准备后：

```powershell
.\scripts\Test.ps1 -Quality
.\scripts\Test-InstallerDeployment.ps1
```

核心、窗口及交互回归共 127 项，另有中英文 OCR 与 8 张合成质量样本。报告写到忽略目录 `reports`，不提交公开仓库。交互回归也包含真实异步 OCR；`-SkipOcr` 只跳过独立 OCR 套件，不能代替完整的身份准备。

`Test-InstallerPrerequisites.ps1` 的载荷替身检查需要构建安装器产生的公共测试证书。完成下一节打包后可执行：

```powershell
.\scripts\Test-InstallerPrerequisites.ps1
```

两组安装脚本检查共 30 项，模拟系统修改，不会实际下载语言或注册包；它们不能代替真实机器安装验收。

## 生成试用 EXE

```powershell
.\scripts\Build-Installer.ps1
```

脚本隔离编译发行程序，生成测试证书并签名程序和小型身份包，使用 Inno Setup 生成 `out\release\SnipLeaf-0.1.4-x64-Preview-Setup.exe`。可用 `-InnoCompiler` 指定 `ISCC.exe` 的位置。

打包只生成文件，不安装、不信任证书、不修改系统 OCR 语言；临时签名私钥随后删除。同版本安装器已存在时会拒绝覆盖。自编译产物使用新的测试证书，不能宣称与 Releases 的归档安装包逐字节一致；重新分发还需处理自身签名和部署身份。

正式公众发行需要受 Windows 信任的代码签名，或适当的商店分发流程。仓库不包含生产签名凭据。
