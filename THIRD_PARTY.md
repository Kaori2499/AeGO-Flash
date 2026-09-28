# AeGO Flash 第三方组件与许可

安装包中的 `AeGOFlash.aex` 静态链接 Live2D Cubism Core 和 Framework，并附带 Framework 着色器。源码包不包含完整 SDK 或用户模型。

## Live2D Cubism SDK for Native

使用 Cubism 5 SDK for Native R5，Framework 为 `5-r.5`。

- [官方 SDK](https://www.live2d.com/en/sdk/download/native/)
- [Framework 源码](https://github.com/Live2D/CubismNativeFramework/tree/5-r.5)
- Core 为专有软件；Framework 和着色器使用 Live2D Open Software License。
- 安装包完整保留 SDK 许可原文：`licenses/Core-LICENSE.md`、`licenses/Framework-LICENSE.md`。使用和分发须遵守其中条款；本说明不替代许可原文，也不授予模型或 SDK 的额外权利。

构建时在 Framework 副本中加入物理状态重置、Core 原位重置和临时绘制排序支持。这些是本项目的扩展，不修改原 SDK 文件或 Core 二进制。

## Adobe After Effects C++ SDK

使用 Adobe 提供的头文件和接口构建原生效果。SDK 由开发者自行从 [Adobe 官方入口](https://developer.adobe.com/after-effects/) 取得，并遵守随附条款；不随源码包分发完整 SDK。

## Windows

使用 Windows SDK 的 Direct3D 11、DXGI、D3DCompiler、WIC 和 Win32 接口。运行插件不需要另装 Node.js、Python、Qt 或 CEP。

## 模型与素材

模型、纹理、动作、表情和音频由用户提供，权利归各自权利人。安装包不附带用户模型，也不改变素材的使用或分发许可。
