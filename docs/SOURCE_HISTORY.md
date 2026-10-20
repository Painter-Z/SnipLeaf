# 源码版本与历史

GitHub 的 `main` 显示当前代码，每次提交保留差异；标签固定一个可查看和下载的仓库快照。更新 `main` 不会删除旧提交。

## 目前可用的版本

| 入口 | 应用源码版本 | 说明 |
| --- | --- | --- |
| [main](https://github.com/Painter-Z/SnipLeaf/tree/main) | 0.1.11 | 当前开发起点，后续可能继续更新 |
| [source-v0.1.11](https://github.com/Painter-Z/SnipLeaf/tree/source-v0.1.11) | 0.1.11 | 本次公开的完整新版源码快照 |
| [v0.1.4](https://github.com/Painter-Z/SnipLeaf/tree/v0.1.4) | 0.1.4 | 首次公开的完整应用源码 |
| [source-v0.1.4](https://github.com/Painter-Z/SnipLeaf/tree/source-v0.1.4) | 0.1.4 | 同步新版前的完整仓库备份，包含后来更新的使用文档 |
| v0.1.5–v0.1.7、v0.1.11 | 0.1.4 | 首次上传对应安装包时只更新文档，标签保留当时状态 |
| v0.1.0–v0.1.3 | 无已核实的应用源码快照 | 只有原始安装包与发布文档，没有补造旧版代码 |

本次保留所有已有提交和标签；`source-v0.1.4` 指向更新源码前的提交，`source-v0.1.11` 指向新版源码提交。0.1.8–0.1.10 没有独立的公开源码快照。

## 下载最新源码

- 在 [0.1.11 发布页](https://github.com/Painter-Z/SnipLeaf/releases/tag/v0.1.11) 的 Assets 下载 `SnipLeaf-0.1.11-source.zip` 和专用的 `SnipLeaf-0.1.11-source-SHA256SUMS.txt`。
- 或打开 [source-v0.1.11](https://github.com/Painter-Z/SnipLeaf/tree/source-v0.1.11)，使用 Code → Download ZIP。
- 使用 Git 可执行 `git clone --branch source-v0.1.11 https://github.com/Painter-Z/SnipLeaf.git`。查看旧版时可在自己的克隆中切换至 `v0.1.4` 或 `source-v0.1.4`；切换前先保存本地修改。

`v0.1.11` 发布页自动生成的 Source code 附件跟随原有标签，仍是旧应用源码。为保留历史，没有移动该标签；请使用上面两个最新源码入口。

## 源码包范围

包含完整 C++ 应用源码、原创图标与资源、CMake 配置、开发和安装器脚本、自动检查代码、MIT 许可证及使用／构建文档。安装包原始 EXE 不放入源码 ZIP。

不包含编译器和 SDK、构建产物、个人截图或 OCR 正文、本机设置、翻译账户文件、证书私钥、研究工具和其他软件的解包文件。翻译专项中的凭据是公开示例，仅用于本地测试，软件运行时每位用户自行配置账户。

源码快照与安装包均为 0.1.11 功能版本；重新编译会因时间戳、构建路径与重新生成的测试签名而产生不同二进制文件，不承诺安装包逐字节可复现。构建与验证步骤见 [构建说明](BUILD.md)。
