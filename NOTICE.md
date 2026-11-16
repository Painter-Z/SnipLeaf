# 资源与第三方说明

SnipLeaf 自身的源码、脚本、文档和原创图标按仓库 [MIT 许可证](LICENSE) 提供。允许使用、修改、再分发和商用，需要保留许可证及版权声明；标准文本见 [Open Source Initiative](https://opensource.org/license/mit)。

应用使用 Windows 原生 API，包括 Win32、Direct2D、DirectWrite、Windows Imaging Component 和 Windows OCR；仓库不包含 Windows SDK、系统 OCR 模型、系统字体或 Visual Studio 的安装文件。这些组件由 Microsoft 提供，按其相应条款使用。

EXE 安装器由 Inno Setup 编译，安装器源脚本位于 `packaging/installer`。Inno Setup 编译器独立安装，并适用其自身许可；本仓库不包含该编译器。自编译和再分发时应核对所用版本的 [Inno Setup 许可](https://jrsoftware.org/files/is/license.txt)。

图标由 `scripts/Generate-Assets.ps1` 绘制，示例图片来自合成文字与标注。仓库不分发 Snipaste 或其他截图软件的程序、源码、图标和解包资源。SnipLeaf 是独立实现的项目，与 Snipaste 无隶属关系。

历史安装包按原始字节归档，保留构建时的试用说明和专用自签名证书。测试证书不是公众信任的生产签名，仓库不包含证书私钥。源码开源不会改变历史安装包的签名、语言准备行为或适用环境。

0.1.14 及更新版安装包包含 Sdcb.SimdPaddleOCR 2.0.0、ModelProvider 1.0.0、ChineseV6Small 1.0.0 和 TextLineOrientation 1.0.0，使用 Apache-2.0；模型来源为 PaddleOCR，推理引擎上游为 [SimdPaddleOCR](https://github.com/sdcb/SimdPaddleOCR)。内置 Native AOT 组件包含 .NET 10.0.15 的运行时部分，适用 MIT 和其第三方声明。完整许可文件随安装包安装在 `Licenses/`，不因 SnipLeaf 自身采用 MIT 而改为 MIT。

本次只发布新版安装包与使用文档，公开应用源码仍是 0.1.12，尚不含该增强组件。用户不需要单独安装 .NET 或联网下载模型；增强 OCR 在本机处理图片。

软件不附带 Microsoft 或其他第三方组件的额外授权；MIT 授权适用于 SnipLeaf 自身内容。
