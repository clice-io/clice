# Build from Source

clice 依赖 C++23 特性，需要使用高版本的 C++ 编译器。同时，我们需要链接 LLVM/Clang 库来解析 AST。为了加快构建速度，默认配置会下载我们发布的 [clice-llvm](https://github.com/clice-io/clice-llvm) 预编译包。这要求你的本地环境与预编译环境保持较高的一致性（尤其是开启 Address Sanitizer 或 LTO 时）。

为了简化环境设置并保证可复现性，我们**强烈推荐**使用 [pixi](https://pixi.prefix.dev/latest) 来管理开发环境。所有的依赖版本均严格定义在 `pixi.toml` 中。

如果你不想使用 pixi，请参考下方的 [Manual Build](#manual-build) 章节。

## Quick Start

请参考 [pixi](https://pixi.prefix.dev/latest/installation) 官方指南安装 pixi。

我们内置了一系列任务，以下命令可直接完成编译并运行测试：

```shell
# configure && build (default RelWithDebInfo)
pixi run build

# unit && integration
pixi run test
```

细粒度任务：上述命令由多个子任务组成，你也可以单独运行它们，并支持通过第一个参数指定构建类型：

```shell
pixi run cmake-config Debug
pixi run cmake-build Debug
pixi run unit-test Debug
pixi run integration-test Debug
```

> [!TIP]
> 如果你想直接使用 `cmake`, `ninja`, `clang++` 等命令进行开发，请运行 `pixi shell` 进入已配置好环境变量的终端

### XMake

我们同样支持使用 XMake 构建：

```shell
# config & build (default releasedbg)
pixi run xmake

# unit & integration
pixi run xmake-test
```

## Manual Build

如果你打算手动构建，请务必先确认你的工具链满足 pixi.toml 中定义的版本要求。

> 兼容性说明：理论上 clice 不依赖特定编译器的扩展，可以使用主流编译器（GCC/Clang/MSVC）编译。但我们仅在 CI 中保证特定版本的 Clang 能通过测试。对于其他编译器或版本，我们提供**尽力而为 (Best Effort)** 的支持。如果遇到问题，欢迎提交 Issue 或 PR

### CMake

```shell
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake \
    -DCLICE_ENABLE_TEST=ON
```

> 注意：`CMAKE_TOOLCHAIN_FILE` 是可选的。如果你使用的工具链与我们完全一致，可以使用预定义的 `cmake/toolchain.cmake`，否则请移除该选项

可选的构建选项：

| 选项                 | 默认值 | 效果                                                                                               |
| -------------------- | ------ | -------------------------------------------------------------------------------------------------- |
| LLVM_INSTALL_PATH    | ""     | 使用自定义路径的 LLVM 库来构建 clice                                                               |
| CLICE_ENABLE_TEST    | OFF    | 是否构建 clice 的单元测试                                                                          |
| CLICE_USE_LIBCXX     | OFF    | 是否使用 libc++ 来构建 clice（添加 `-std=libc++`），如果开启，请确保 LLVM 库也是使用 libc++ 编译的 |
| CLICE_CI_ENVIRONMENT | OFF    | 是否打开 `CLICE_CI_ENVIRONMENT` 这个宏，有些测试在 CI 环境才会执行                                 |

### XMake

使用如下命令即可构建 clice：

```bash
xmake f -c --mode=releasedbg --toolchain=clang
xmake build --all
```

可选的构建选项：

| 选项          | 默认值 | 效果                                 |
| ------------- | ------ | ------------------------------------ |
| --llvm        | ""     | 使用自定义路径的 LLVM 库来构建 clice |
| --enable_test | false  | 是否构建 clice 的单元测试            |
| --ci          | false  | 是否打开 `CLICE_CI_ENVIRONMENT`      |

## About LLVM

clice 调用 Clang API 来解析 C++ 代码，因此必须链接 LLVM/Clang 库。由于 clice 使用了 Clang 的私有头文件（这些文件通常不包含在发行版中），不能直接使用系统安装的 LLVM 包。

主要有两种方式解决这个依赖问题：

1. 我们在 [clice-llvm](https://github.com/clice-io/clice-llvm/releases) 上会发布使用的 LLVM 版本的预编译二进制，用于 CI 或者 release 构建。在构建时 cmake 和 xmake 默认会从此处下载 LLVM 库然后使用。

> [!IMPORTANT]
>
> 对于 debug 版本的 LLVM 库，构建的时候我们开启了 address sanitizer，而 address sanitizer 依赖于 compiler rt，它对编译器版本十分敏感。所以如果使用 debug 版本，请确保你的 clang 的 compiler rt 版本与 `pixi.toml` 中的定义严格一致。

2. 自行构建一套与当前环境一致的 LLVM/Clang。如果默认的预编译二进制文件在你的系统上因 ABI 或库版本不兼容而运行失败，或者你需要一个自定义的 Debug 版本，那么我们推荐你使用此方法从头编译 LLVM 库。我们提供了一个脚本 `scripts/build-llvm.py` 用于构建所需要的 LLVM 库，也可以参考 LLVM 的官方构建教程 [Building LLVM with CMake](https://llvm.org/docs/CMake.html)。
