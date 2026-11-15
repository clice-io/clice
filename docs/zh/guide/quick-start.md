# 快速开始

## 编辑器插件

clice 实现了[语言服务器协议](https://microsoft.github.io/language-server-protocol)。任何支持该协议的编辑器都可与 clice 配合，提供 `code completion`、`diagnostics`、`hover`、`semantic highlighting` 等功能。

除标准协议外，clice 还支持若干协议扩展。为获得更完整的集成体验，建议使用专用的 clice 插件。

各编辑器的配置说明见[编辑器设置](./editors.md)：Visual Studio Code、Neovim 和 Zed 提供官方插件，使用通用 LSP 客户端的编辑器（Helix、Emacs、Sublime Text、Kate、Vim）则提供配置片段。

## 安装

### 下载预编译二进制文件

从 [Releases](https://github.com/clice-io/clice/releases) 页面下载最新二进制文件。

### 从源码构建

详细步骤参见[从源码构建](./build.md)。

## 项目配置

要让 clice 正确理解代码（例如定位头文件），需要提供 `compile_commands.json` 文件，即[编译数据库](https://clang.llvm.org/docs/JSONCompilationDatabase.html)。编译数据库记录每个源文件的编译选项。

默认情况下，clice 会加载项目根目录及其直接子目录（如 `build/`）中的每一份 `compile_commands.json`，并在打开某个文件时加载该文件到项目根目录之间各级目录中的编译数据库，因此目录树深处的数据库，会在其所在目录或更深层目录中的文件首次打开时加载。带有独立 `clice.toml` 或 `.clice/config.toml` 的子目录视为独立项目，外层项目绝不会加载它的数据库。只要任意规则设置了 `compile_commands` 或 `default_command`，这项自动发现就会关闭。如何在 `clice.toml` 中指定编译数据库、如何编译没有数据库条目的文件，参见[配置指南](./config-guide.md#compile-commands)。

### CMake

配置时添加 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`：

```shell
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

这会在 `build` 目录中生成 `compile_commands.json`。

::: warning
此选项仅适用于 Makefile 和 Ninja 生成器。使用其他生成器（如 Visual Studio）时，请改用下方的替代方案。
:::

### Bazel

使用 clice 的 Bazel registry 中的 `compdb` 模块。在项目中添加该 registry 与模块：

```
# .bazelrc
common --registry=https://bazel.clice.io/
common --registry=https://bcr.bazel.build/
```

```python
# MODULE.bazel
bazel_dep(name = "compdb", version = "0.1.0", dev_dependency = True)
```

随后在 `MODULE.bazel` 同级目录生成 `compile_commands.json`：

```bash
bazel run @compdb//:refresh -- --output_groups=+compilation_prerequisites_INTERNAL_
```

默认覆盖 `//...`；在 `--` 后追加目标可缩小范围。附加的输出组（output group）会构建编译命令需要读取但本身不会触发构建的产物：生成的头文件，以及 Bazel 用于暴露库头文件的 `_virtual_includes` 目录。若不加该输出组，这些头文件在其他目标构建它们之前都会缺失，所有包含它们的文件都会报找不到头文件。

### Visual Studio

在 Visual Studio（2019 16.1 及以上版本）中使用 CMake 项目时，在 `CMakeSettings.json` 中配置：

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

在运行 `bear -- make` 前先执行 `make clean`，以确保捕获所有编译命令。

### Meson

Meson 会自动生成编译数据库：

```bash
meson setup build
```

### Xmake

```bash
xmake project -k compile_commands --lsp=clangd build
```

也可以配置 Xmake 的 VS Code 扩展自动生成编译数据库：

```json
"xmake.compileCommandsDirectory": "build"
```

### 其他构建系统

使用其他构建系统时，可借助 [catter](https://github.com/clice-io/catter)——它采用伪编译器方案，适用于任意构建系统。

## 预构建索引（可选）

在编辑器中打开项目时，后台索引会自动启动。大型项目也可以提前在命令行构建索引：

```bash
clice index --workspace /path/to/project
```

构建过程支持断点续跑——按 Ctrl-C 中断时会保存当前进度，下次运行将从中断处继续，而无需重新开始。运行 `clice index stats --workspace /path/to/project` 可查看持久化索引的统计内容。如何缩短首次索引的时间，参见[加快首次索引](./config-guide.md#a-faster-first-index)。
