# 快速开始

## 编辑器插件

clice 实现了 [语言服务器协议](https://microsoft.github.io/language-server-protocol)。任何支持该协议的编辑器均可与 clice 配合使用，提供 `code completion`、`diagnostics`、`hover`、`semantic highlighting` 等功能。

除了标准协议之外，clice 还支持一些协议扩展。为了更好地集成，建议使用专用的 clice 插件。

各编辑器的具体配置见 [编辑器配置](./editors.md)：Visual Studio Code、Neovim 和 Zed 有官方插件，使用通用 LSP 客户端的编辑器（Helix、Emacs、Sublime Text、Kate、Vim）则提供了配置片段。

## 安装

### 下载预编译二进制文件

从 [Releases](https://github.com/clice-io/clice/releases) 页面下载最新二进制文件。

### 从源码构建

参见 [从源码构建](../dev/build.md) 了解详细步骤。

## 项目配置

为了让 clice 正确理解你的代码（例如找到头文件的位置），你需要提供 `compile_commands.json` 文件，也称为 [编译数据库](https://clang.llvm.org/docs/JSONCompilationDatabase.html)。编译数据库为每个源文件提供编译选项。

默认情况下，clice 会在工作区根目录及其每个一级子目录（例如 `build/`）中查找 `compile_commands.json`，并使用找到的第一个文件。你可以通过 [clice.toml](./configuration.md) 中的 `compile_commands_paths` 选项指定确切路径。

### CMake

在配置时添加 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`：

```shell
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

这会在 `build` 目录中生成 `compile_commands.json`。

::: warning
此选项仅适用于 Makefile 和 Ninja 生成器。对于其他生成器（例如 Visual Studio），请使用下面的替代方法。
:::

### Bazel

使用 [bazel-compile-commands-extractor](https://github.com/hedronvision/bazel-compile-commands-extractor)：

```bash
bazel run @hedron_compile_commands//:refresh_all
```

### Visual Studio

对于 Visual Studio 2019 16.1 及以上版本中的 CMake 项目，请在 `CMakeSettings.json` 中配置：

```json
{
  "configurations": [
    {
      "name": "x64-Debug",
      "generator": "Ninja",
      "buildRoot": "${projectDir}\\build",
      "cmakeCommandArgs": "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    }
  ]
}
```

对于 MSBuild 项目（`.vcxproj`），使用 [catter](https://github.com/clice-io/catter) 生成编译数据库。

### Makefile

使用 [bear](https://github.com/rizsotto/Bear) 拦截编译命令：

```bash
bear -- make
```

在 `bear -- make` 之前运行 `make clean`，以确保捕获所有命令。

### Meson

Meson 会自动生成编译数据库：

```bash
meson setup build
```

### Xmake

```bash
xmake project -k compile_commands --lsp=clangd build
```

或者配置 Xmake VS Code 扩展，使其自动生成编译数据库：

```json
"xmake.compileCommandsDirectory": "build"
```

### 其他构建系统

对于其他任何构建系统，都可以使用 [catter](https://github.com/clice-io/catter)——这是一种适用于所有构建系统的伪编译器方案。

## 预构建索引（可选）

在编辑器中打开项目时，后台索引会自动启动。
对于大型项目，你也可以通过命令行
提前构建索引：

```bash
clice index --workspace /path/to/project
```

该过程支持断点续作——用 Ctrl-C 中断时会保存进度，下次运行会从上次中断处
继续，而不是从头开始。
`clice index --stats` 会输出当前持久化索引所包含的内容。
