# 从源码构建

clice 依赖 C++23 特性，需要使用现代 C++ 工具链。同时还需要链接 LLVM/Clang 以解析 AST。两者都来自 clice-io 的 clang 工具链 [xclang](https://github.com/clice-io/xclang)：[Bazel](https://bazel.build) 使用 xclang 的工具链构建 clice，并链接同一 xclang 版本预编译的 LLVM/Clang 库，工具链和库都由 Bazel 自行下载。二者必须匹配：这些库里是 ThinLTO bitcode，只有该版本的工具链能读取。

为了简化环境配置并确保构建可复现，我们**强烈推荐**使用 [pixi](https://pixi.prefix.dev/latest) 管理开发环境。依赖版本固定在 `pixi.toml` 中；Bazel 的依赖版本固定在 `MODULE.bazel` 中。

## 快速开始

请按照[官方指南](https://pixi.prefix.dev/latest/installation)安装 pixi。

我们提供了多项任务；以下命令会构建并运行测试：

```shell
# build (default RelWithDebInfo) into build/RelWithDebInfo
pixi run build

# unit + integration + smoke + snap tests
pixi run test
```

如需使用粒度更细的任务（第一个参数用于指定构建类型）：

```shell
pixi run build Debug
pixi run unit-test Debug
pixi run integration-test Debug
pixi run smoke-test Debug
pixi run snap-test Debug
```

`pixi run build` 用 Bazel 构建，并把程序放到 `build/<type>/bin` 中，与之并列的是资源目录 `build/<type>/lib/clang`，clice 从中读取 clang 的头文件；测试和编辑器都从这里运行这些程序。

> [!TIP]
> 运行 `pixi shell` 可进入已配置好所有环境变量的 shell，在其中用 `npx bazel` 直接调用 Bazel。

## Bazel

Bazel 通过 [bazelisk](https://github.com/bazelbuild/bazelisk) 运行。bazelisk 是仓库中的一个 npm 包（由 `npm ci` 安装），它会下载 `.bazelversion` 指定的 Bazel 版本：

```shell
npx bazel build //:clice //:unit_tests
```

构建类型即 `.bazelrc` 中定义的配置：

| 配置                      | 作用                                                                                                                   |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `--config=RelWithDebInfo` | 默认配置：开启优化，带调试信息                                                                                         |
| `--config=Debug`          | 不开启优化，启用 Address Sanitizer；Windows 上不启用                                                                   |
| `--config=release`        | 像发布 clice 时那样，用 LLVM/Clang 库的 ThinLTO bitcode 链接各个程序；其他构建链接的是已编译为本机代码的库，速度快得多 |

`--` 之后的选项会经由 `pixi run build` 传给 Bazel，例如 `pixi run build RelWithDebInfo -- --config=release`。

`pixi run compile-commands` 会在仓库根目录写出 clice 自身源码的 `compile_commands.json`，这样 clice 也能用于开发它自己的代码。

在 Windows 上，Bazel 默认的输出根目录层级过深，超出了 Windows 路径的限制；请在 `%USERPROFILE%\.bazelrc` 中指定一个较短的路径：

```
startup --output_user_root=C:/b
```

在 Windows 上，Bazel 还需要 Bash，可由 [Git for Windows](https://gitforwindows.org) 提供。

## 关于 LLVM

clice 调用 Clang API 解析 C++ 代码，因此必须链接 LLVM/Clang。由于 clice 使用 Clang 的私有头文件（发行版软件包通常不包含这些文件），因此无法直接使用系统提供的 LLVM 软件包。

每个 [xclang](https://github.com/clice-io/xclang/releases) 版本都会为全部六个目标发布预编译的 LLVM/Clang 库（`libclang-*` 压缩包），由该版本自己的工具链构建；xclang 的 Bazel 模块把这些库声明为仓库（repository），Bazel 会连同工具链一起下载它们。

> [!IMPORTANT]
>
> 调试构建会启用 Address Sanitizer，并链接 xclang 为 x86_64 Linux 和 arm64 macOS 发布的 ASan 插桩库；arm64 Linux 和 x86_64 macOS 没有这类库，也没有调试构建。Windows 的调试构建链接发布版的库，不启用 Address Sanitizer。

自行构建的 LLVM/Clang 可通过 `--repo_env=XCLANG_LIBCLANG_ROOT=<directory>` 替换发布版的库（ASan 版本用 `XCLANG_LIBCLANG_ASAN_ROOT`）；它必须按 xclang 的方式构建，即使用 xclang 的 `scripts/toolchain.ts`；参见 [xclang](https://github.com/clice-io/xclang)。
