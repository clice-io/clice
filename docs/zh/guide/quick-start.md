# Quick Start

## 编辑器插件

clice 实现了 [Language Server Protocol](https://microsoft.github.io/language-server-protocol)，任何支持该协议的编辑器均可以与 clice 配合使用，提供 `代码补全`、`诊断`、`悬停信息`、`语义高亮` 等功能。

除了标准协议之外，clice 还支持一些协议扩展。为了更好地处理这些扩展以及更好地与编辑器集成，使用专用的 clice 插件是推荐方案。

### Visual Studio Code

从插件市场安装 [clice](https://marketplace.visualstudio.com/items?itemName=ykiko.clice-vscode) 插件。它会自动下载 clice 二进制文件。

如需使用自定义二进制文件，在工作区设置中配置 `clice.executable`：

```jsonc
{
  "clice.executable": "/path/to/clice"
}
```

### Vim/Neovim

将 clice Neovim 插件添加到 runtime path：

```lua
-- lazy.nvim
{ dir = "/path/to/clice/editors/nvim" }

-- 或手动添加
vim.opt.rtp:append("/path/to/clice/editors/nvim")
```

### 其它编辑器

其它编辑器暂时还没有专用的 clice 插件（欢迎贡献！）。请自行安装 clice 二进制文件，并配置编辑器的 LSP 客户端运行 `clice server`。

## 安装

### 下载预编译二进制

从 [Releases](https://github.com/clice-project/clice/releases) 页面下载最新版本。

### 从源码构建

参见 [从源码构建](../dev/build.md) 了解详细步骤。

## 项目配置

为了让 clice 能正确理解你的代码（例如找到头文件的位置），需要提供一份 `compile_commands.json` 文件，即 [编译数据库](https://clang.llvm.org/docs/JSONCompilationDatabase.html)。

默认情况下，clice 在工作区的 `build/` 目录中查找 `compile_commands.json`。可以通过 [clice.toml](./configuration.md) 中的 `compile_commands_paths` 选项自定义。

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
