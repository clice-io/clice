# 快速开始

## 编辑器插件

clice 实现了 [Language Server Protocol](https://microsoft.github.io/language-server-protocol)。任何支持该协议的编辑器均可与 clice 配合使用，提供 `code completion`、`diagnostics`、`hover`、`semantic highlighting` 等功能。

除了标准协议之外，clice 还支持一些协议扩展。为了更好地集成，建议使用专用的 clice 插件。

各编辑器的具体配置见 [Editor Setup](./editors.md)：Visual Studio Code、Neovim 和 Zed 有官方插件，Helix、Emacs、Sublime Text、Kate、Vim 等通用 LSP 客户端提供配置片段。

## 安装

### 下载预编译二进制

从 [Releases](https://github.com/clice-io/clice/releases) 页面下载最新二进制。

### 从源码构建

参见 [从源码构建](../dev/build.md) 了解详细步骤。

## 项目配置

为了让 clice 正确理解你的代码（例如找到头文件位置），你需要提供一份 `compile_commands.json` 文件，即 [编译数据库](https://clang.llvm.org/docs/JSONCompilationDatabase.html)。编译数据库为每个源文件提供编译选项。

默认情况下，clice 会搜索工作区根目录及其每个直接子目录（例如 `build/`）中的 `compile_commands.json`，使用找到的第一个。你可以通过 [clice.toml](./configuration.md) 中的 `compile_commands_paths` 选项指定确切路径。

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

对于 Visual Studio（2019 16.1+）中的 CMake 项目，在 `CMakeSettings.json` 中配置：

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

Meson 自动生成编译数据库：

```bash
meson setup build
```

### Xmake

```bash
xmake project -k compile_commands --lsp=clangd build
```

或者配置 Xmake VS Code 扩展自动生成：

```json
"xmake.compileCommandsDirectory": "build"
```

### 其它

对于任何其他构建系统，可以使用 [catter](https://github.com/clice-io/catter) — 一种伪装编译器的方法，适用于任何构建系统。

## 预构建索引（可选）

在编辑器中打开项目时，后台索引会自动启动。
对于大型项目，你也可以从命令行提前
构建索引：

```bash
clice index --workspace /path/to/project
```

运行可恢复 — 用 Ctrl-C 中断会保存进度，下次调用会从上次中断处
继续，而不是重新开始。
`clice index --stats` 会打印当前持久化索引包含的内容。
