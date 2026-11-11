# 语言服务器概览

clice 提供一套基于 LLVM/Clang 构建的 C++ 开发工具。本节介绍已实现和计划中的功能，并提供相关上游问题的链接。

## 语言

clice 支持 C、C++ 和 CUDA；对 Objective-C 和 Objective-C++，则支持阅读代码：语义 Token、悬停、代码导航与查找引用、调用层次结构与类型层次结构、大纲、折叠范围、工作区符号以及 `clice query` 都认得其中的类、协议、分类（category）、方法、属性和实例变量，重命名也能处理类、协议、分类和实例变量。不支持重命名方法或属性，也不支持补全消息；Clang 的头文件模块（`-fmodules`、`@import`）同样不支持：文件会在不启用它们的情况下编译，clice 对此只提示一次。和所有项目一样，Objective-C 项目也需要编译数据库：CMake 的 Makefile 和 Ninja 生成器会生成，Xcode 和 CMake 的 Xcode 生成器都不会。

OpenCL C 和 C++ for OpenCL（`.cl`、`.clcpp`）按 Clang 的方式编译，内建函数也都认得；编译数据库没有列出的文件，取规则的 `default_command`，例如 `clang -cl-std=CL3.0`。VS Code 扩展为这两种扩展名提供 `opencl` 语言；`.cl` 也是 Common Lisp 的扩展名，可以用 `files.associations` 改回去。

## 功能

将 clice 用作编辑器后端时可用的语言服务器协议功能。

<!-- The status matrix counts the documented cases each feature page shows,
     in the order of the sidebar. Do not edit the region between the
     GENERATED markers by hand — run `pixi run update-feature-docs`. -->

<!-- BEGIN GENERATED OVERVIEW -->

| 功能                                 | 状态                                     |
| ------------------------------------ | ---------------------------------------- |
| [代码补全](./completion.md)          | 支持 49 项                               |
| [悬停](./hover.md)                   | 支持 72 项                               |
| [签名帮助](./signature-help.md)      | 支持 21 项 · 部分支持 1 项 · 不支持 2 项 |
| [代码导航](./navigation.md)          | 支持 115 项 · 不支持 8 项                |
| [重命名](./rename.md)                | 支持 9 项                                |
| [诊断](./diagnostics.md)             | 支持 10 项 · 部分支持 1 项               |
| [代码操作](./code-action.md)         | 支持 38 项                               |
| [语义 Token](./semantic-tokens.md)   | 支持 61 项                               |
| [内联提示](./inlay-hints.md)         | 支持 40 项                               |
| [文档链接](./document-links.md)      | 支持 5 项 · 部分支持 1 项                |
| [文档符号](./document-symbols.md)    | 支持 29 项                               |
| [折叠范围](./folding-ranges.md)      | 支持 24 项                               |
| [选择范围](./selection-ranges.md)    | 支持 14 项                               |
| [格式化](./formatting.md)            | 支持 4 项                                |
| [C++20 模块](./modules.md)           | 支持 9 项                                |
| [头文件上下文](./header-contexts.md) | 支持 6 项                                |
| [编译](./compilation.md)             | 支持 8 项                                |

<!-- END GENERATED OVERVIEW -->

## 图例

每项功能均按状态统计其页面中记录的能力数量：

- **支持** — 该能力可用；快照（snapshot）锁定其行为
- **部分支持** — 尚不完整；快照锁定目前可用的行为
- **不支持** — 已记录的功能缺口，已跟踪但尚未实现
