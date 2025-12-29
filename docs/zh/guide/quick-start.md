# Quick Start

## Editor Plugins

clice 实现了 [Language Server Protocol](https://microsoft.github.io/language-server-protocol)，任何支持该协议的编辑器原则上均可以与 clice 一起使用，提供像 `code completion`, `diagnostics`, `go-to-definition`, 等等。

但是除了标准协议之外，clice 还支持一些协议扩展，为了更好的处理这些协议扩展以及更好的与编辑器集成。使用特定编辑器中的 clice 插件往往是更好的选择，它们大多数都是开箱即用的，并且支持 clice 的协议扩展。

### Visual Studio Code

### Vim/Neovim

### Others

其它的编辑器还没有可用的 clice 插件（欢迎贡献！），为了在它们中使用 clice，请自行安装 clice 并参考特定编辑器的文档关于如何使用一个语言服务器。

## Installation

如果你的编辑器插件负责了 clice 的下载，可以跳过这一步。

### Download Prebuilt Binary

通过 Release 界面下载 clice 二进制版本。

### Build from Source

自己从源码编译 clice，具体的步骤参考 [build](../dev/build.md)。

## Project Setup

为了让 clice 能正确理解你的代码（例如找到头文件的位置），需要为 clice 提供一份 `compile_commands.json` 文件，也就说所谓的 [编译数据库](https://clang.llvm.org/docs/JSONCompilationDatabase.html)。编译数据库中提供了每个源文件的编译选项。

### CMake

对于使用 cmake 的构建系统来说，在构建的时候添加 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` 选项即可，例如

```cmake
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

这会在 `build` 目录下生成一份 `compile_commands.json` 文件。

::: warning
注意：只有当 cmake 的生成器选择为 makefile 和 ninja 的时候，这个选项才有作用。对于其它生成器会忽略这个选项，也就是说无法生成编译数据库。
:::

### Bazel

Bazel 不支持直接生成编译数据库，推荐使用 [bazel-compile-commands-extractor](https://github.com/hedronvision/bazel-compile-commands-extractor)。在安装好之后，你可以这样生成 `compile_commands.json`:

```bash
bazel run @hedron_compile_commands//:refresh_all
```

### Visual Studio

TODO:

### Makefile

TODO:

### Xmake

### Others

对于任意其它的构建系统，可以尝试使用 [bear](https://github.com/rizsotto/Bear) 或者 [scan-build](https://github.com/rizsotto/scan-build) 来拦截编译命令并获取到编译数据库（不保证成功）。我们计划在未来编写一个**新的工具**，通过假编译器的方式来实现编译命令的捕获。
