# 语言服务器概览

clice 提供一套基于 LLVM/Clang 构建的 C++ 开发工具。本节按功能逐一介绍 clice 的能力，以及每项功能解决的 clangd 问题。

## 语言

clice 支持 C、C++、CUDA、HIP 和 OpenCL；对 Objective-C 和 Objective-C++，则支持代码阅读功能：语义 Token、悬停、代码导航与查找引用、调用层次结构与类型层次结构、大纲、折叠范围、工作区符号以及 `clice query` 均能识别其中的类、协议、分类（category）、方法、属性和实例变量，代码补全能写出消息的选择器（selector），重命名支持类、协议、分类和实例变量。不支持重命名方法或属性，也不支持 Clang 头文件模块（`-fmodules`、`@import`）：文件会在不启用头文件模块的情况下编译，clice 对此只提示一次。与其他项目一样，Objective-C 项目也需要编译数据库；CMake 的 Makefile 和 Ninja 生成器会生成它，而 Xcode 和 CMake 的 Xcode 生成器都不会。

CUDA 和 HIP 文件编译的是设备端代码，因为 Clang 会先执行这一步。OpenCL C 和 C++ for OpenCL（`.cl`、`.clcpp`）直接按 Clang 的方式编译，内建函数也都可用；编译数据库未列出的文件采用规则的 `default_command`，例如 `clang -cl-std=CL3.0`。VS Code 本身没有内置这些语言，VS Code 扩展会为 `.hip` 提供 `hip` 语言，为各 OpenCL 扩展名提供 `opencl` 语言；`.cl` 同时也是 Common Lisp 的扩展名，可用 `files.associations` 改回。

## 功能

将 clice 用作编辑器后端时可用的语言服务器协议（LSP）功能。

<!-- The status matrix counts the documented cases each feature page shows,
     in the order of the sidebar. Do not edit the region between the
     GENERATED markers by hand — run `pixi run docs-update`. -->

<!-- BEGIN GENERATED OVERVIEW -->

| 功能                                 | 状态                                      |
| ------------------------------------ | ----------------------------------------- |
| [代码补全](./completion.md)          | 支持 55 项 · 部分支持 1 项                |
| [悬停](./hover.md)                   | 支持 72 项                                |
| [签名帮助](./signature-help.md)      | 支持 21 项 · 部分支持 1 项 · 不支持 2 项  |
| [代码导航](./navigation.md)          | 支持 117 项 · 部分支持 1 项 · 不支持 6 项 |
| [重命名](./rename.md)                | 支持 9 项                                 |
| [诊断](./diagnostics.md)             | 支持 12 项 · 部分支持 1 项                |
| [代码操作](./code-action.md)         | 支持 38 项                                |
| [语义 Token](./semantic-tokens.md)   | 支持 61 项                                |
| [内联提示](./inlay-hints.md)         | 支持 40 项                                |
| [文档链接](./document-links.md)      | 支持 5 项 · 部分支持 1 项                 |
| [文档符号](./document-symbols.md)    | 支持 29 项                                |
| [折叠范围](./folding-ranges.md)      | 支持 24 项                                |
| [选择范围](./selection-ranges.md)    | 支持 14 项                                |
| [格式化](./formatting.md)            | 支持 4 项                                 |
| [C++20 模块](./modules.md)           | 支持 9 项                                 |
| [头文件上下文](./header-contexts.md) | 支持 6 项                                 |
| [编译](./compilation.md)             | 支持 9 项                                 |

<!-- END GENERATED OVERVIEW -->

## 图例

每项功能按状态统计其页面中记录的能力数量：

- **支持**——该能力可用；快照（snapshot）锁定其行为
- **部分支持**——部分可用；快照锁定其中可用的部分
- **不支持**——已记录的功能缺口；快照锁定 clice 的替代行为
