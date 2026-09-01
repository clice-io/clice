# Build from Source

clice 依赖 C++23 特性，需要使用现代 C++ 工具链。同时需要链接 LLVM/Clang 来解析 AST。为了加快构建速度，默认配置会下载我们发布的 [clice-llvm](https://github.com/clice-io/clice-llvm) 预编译包。这要求你的本地环境与预编译环境保持较高的一致性（尤其是开启 Address Sanitizer 或 LTO 时）。

为了简化环境配置并保持构建可复现，我们**强烈推荐**使用 [pixi](https://pixi.prefix.dev/latest) 来管理开发环境。依赖版本固定在 `pixi.toml` 中。

如果你不想使用 pixi，请参考下方的 [Manual Build](#manual-build)。

## 快速开始

请按照 [官方指南](https://pixi.prefix.dev/latest/installation) 安装 pixi。

我们提供了一些任务；以下命令完成配置、构建并运行测试：

```shell
# configure && build (default RelWithDebInfo)
pixi run build

# unit + integration + smoke + snap tests
pixi run test
```

更细粒度的任务（第一个参数指定构建类型）：

```shell
pixi run cmake-config Debug
pixi run cmake-build Debug
pixi run unit-test Debug
pixi run integration-test Debug
pixi run smoke-test Debug
pixi run snap-test Debug
```

> [!TIP]
> 如果你想直接使用 `cmake`、`ninja`、`clang++` 等命令进行开发，请运行 `pixi shell` 进入已配置好环境变量的 shell。

## Manual Build

如果你打算手动构建，请先确保你的工具链与 `pixi.toml` 中定义的版本一致。

> 兼容性说明：理论上 clice 不依赖编译器特定扩展，主流编译器（GCC/Clang/MSVC）应该可以工作。但 CI 只保证特定版本的 Clang。其他编译器或版本按**尽力而为**支持。如果遇到问题，请提交 Issue 或 PR。

### CMake

```shell
cmake -B build/RelWithDebInfo -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake \
    -DCLICE_ENABLE_TEST=ON

cmake --build build/RelWithDebInfo
```

> 注意：`CMAKE_TOOLCHAIN_FILE` 是可选的。如果你的工具链与我们的完全一致，可以使用预定义的 `cmake/toolchain.cmake`；否则请移除此选项。

### CMake 选项

| 选项                   | 默认值 | 效果                                                           |
| ---------------------- | ------ | -------------------------------------------------------------- |
| LLVM_INSTALL_PATH      | ""     | 使用自定义路径的 LLVM 构建 clice                               |
| CLICE_ENABLE_TEST      | OFF    | 构建单元测试和基准测试基础设施                                 |
| CLICE_ENABLE_BENCHMARK | OFF    | 构建基准测试                                                   |
| CLICE_ENABLE_LTO       | OFF    | 为所有目标启用 ThinLTO                                         |
| CLICE_USE_LIBCXX       | OFF    | 使用 libc++（添加 `-stdlib=libc++`）；LLVM 库也必须使用 libc++ |
| CLICE_CI_ENVIRONMENT   | OFF    | 启用 `CLICE_CI_ENVIRONMENT` 宏；部分测试仅在 CI 中运行         |
| CLICE_OFFLINE_BUILD    | OFF    | 禁用配置阶段的网络下载                                         |

## About LLVM

clice 调用 Clang API 解析 C++ 代码，因此必须链接 LLVM/Clang。由于 clice 使用 Clang 的私有头文件（发行版通常不包含），不能直接使用系统 LLVM 包。

满足此依赖有两种方式：

1. 我们在 [clice-llvm](https://github.com/clice-io/clice-llvm/releases) 发布所用 LLVM 版本的预编译二进制，用于 CI 和 release 构建。构建时 cmake 默认会下载这些 LLVM 库。

> [!IMPORTANT]
>
> 对于 debug 版本的 LLVM 构建，我们启用了 address sanitizer，它依赖 compiler-rt，并且对编译器版本非常敏感。如果你使用 debug 构建，请确保你的 clang compiler-rt 版本与 `pixi.toml` 中定义的版本一致。

2. 自行构建 LLVM/Clang 以匹配你的环境。如果默认的预编译二进制因 ABI 或库版本不匹配而失败，或者你需要自定义 debug 构建，请使用这种方式。我们提供 `scripts/build-llvm.py` 来构建所需 LLVM 库，也可以参考 LLVM 官方指南 [Building LLVM with CMake](https://llvm.org/docs/CMake.html)。
