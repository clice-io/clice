# Quick Start

## 编辑器插件

clice 实现了 [Language Server Protocol](https://microsoft.github.io/language-server-protocol)，任何支持该协议的编辑器均可以与 clice 配合使用，提供 `代码补全`、`诊断`、`悬停信息`、`语义高亮` 等功能。

除了标准协议之外，clice 还支持一些协议扩展。为了更好地处理这些扩展以及更好地与编辑器集成，使用专用的 clice 插件是推荐方案。

各编辑器的具体配置见 [Editor Setup](./editors.md)：Visual Studio Code、Neovim 和 Zed 有官方插件，Helix、Emacs、Sublime Text、Kate、Vim 等通用 LSP 客户端提供配置片段。

## 安装

### 下载预编译二进制

从 [Releases](https://github.com/clice-io/clice/releases) 页面下载最新版本。

### 从源码构建

参见 [从源码构建](../dev/build.md) 了解详细步骤。

## 项目配置

为了让 clice 能正确理解你的代码（例如找到头文件的位置），需要提供一份 `compile_commands.json` 文件，即 [编译数据库](https://clang.llvm.org/docs/JSONCompilationDatabase.html)。

默认情况下，clice 会搜索工作区根目录及其各个直接子目录（例如 `build/`），使用找到的第一个 `compile_commands.json`。可以通过 [clice.toml](./configuration.md) 中的 `compile_commands_paths` 选项指定确切路径。

### CMake

构建时添加 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` 选项：

```shell
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

这会在 `build` 目录下生成 `compile_commands.json`。

::: warning
注意：只有当 cmake 的生成器选择为 Makefile 和 Ninja 时此选项才有效。其它生成器会忽略此选项。
:::

### Bazel

使用 [bazel-compile-commands-extractor](https://github.com/hedronvision/bazel-compile-commands-extractor)：

```bash
bazel run @hedron_compile_commands//:refresh_all
```

### Visual Studio

CMake 项目（Visual Studio 2019 16.1+）在 `CMakeSettings.json` 中配置：

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

MSBuild 项目（`.vcxproj`）可使用 [catter](https://github.com/clice-io/catter) 生成编译数据库。

### Makefile

使用 [bear](https://github.com/rizsotto/Bear) 拦截编译命令：

```bash
bear -- make
```

运行前先 `make clean` 以确保捕获所有命令。

### Meson

Meson 自动生成编译数据库：

```bash
meson setup build
```

### Xmake

```bash
xmake project -k compile_commands --lsp=clangd build
```

或配置 Xmake VS Code 扩展自动生成：

```json
"xmake.compileCommandsDirectory": "build"
```

### 其它

对于任意其它的构建系统，可以使用 [catter](https://github.com/clice-io/catter) — 通过伪装编译器的方式来捕获编译命令，能够与任何构建系统配合工作。
