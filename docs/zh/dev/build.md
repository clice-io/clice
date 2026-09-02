# 从源码构建

clice 依赖 C++23 特性，需要使用现代 C++ 工具链。同时还需要链接 LLVM/Clang 以解析 AST。为了加快构建速度，默认配置会下载我们发布的 [clice-llvm](https://github.com/clice-io/clice-llvm) 预编译包。这要求本地环境与预编译环境高度一致（尤其是在启用 Address Sanitizer 或 LTO 时）。

为了简化环境配置并确保构建可复现，我们**强烈推荐**使用 [pixi](https://pixi.prefix.dev/latest) 管理开发环境。依赖版本固定在 `pixi.toml` 中。

如果你不想使用 pixi，请参阅下文的[手动构建](#manual-build)。

## 快速开始

请按照[官方指南](https://pixi.prefix.dev/latest/installation)安装 pixi。

我们提供了多项任务；以下命令会完成配置、构建并运行测试：

```shell
# configure && build (default RelWithDebInfo)
pixi run build

# unit + integration + smoke + snap tests
pixi run test
```

如需使用粒度更细的任务（第一个参数用于指定构建类型）：

```shell
pixi run cmake-config Debug
pixi run cmake-build Debug
pixi run unit-test Debug
pixi run integration-test Debug
pixi run smoke-test Debug
pixi run snap-test Debug
```

> [!TIP]
> 如果你想直接使用 `cmake`、`ninja`、`clang++` 等进行开发，请运行 `pixi shell`，进入已配置好所有环境变量的 shell。

## 手动构建

如果你打算手动构建，请先确保工具链版本与 `pixi.toml` 中定义的版本一致。

> 兼容性说明：理论上，clice 不依赖任何编译器特有的扩展，因此主流编译器（GCC/Clang/MSVC）应该都能使用。不过，CI 仅保证特定版本的 Clang 可用。对于其他编译器或版本，我们只会尽力提供支持。如果遇到问题，请提交 issue 或 PR。

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

| 选项                   | 默认值 | 作用                                                           |
| ---------------------- | ------ | -------------------------------------------------------------- |
| LLVM_INSTALL_PATH      | ""     | 使用自定义路径中的 LLVM 构建 clice                             |
| CLICE_ENABLE_TEST      | OFF    | 构建单元测试和基准测试基础设施                                 |
| CLICE_ENABLE_BENCHMARK | OFF    | 构建基准测试                                                   |
| CLICE_ENABLE_LTO       | OFF    | 为所有目标启用 ThinLTO                                         |
| CLICE_USE_LIBCXX       | OFF    | 使用 libc++（添加 `-stdlib=libc++`）；LLVM 库也必须使用 libc++ |
| CLICE_CI_ENVIRONMENT   | OFF    | 启用 `CLICE_CI_ENVIRONMENT` 宏；部分测试仅在 CI 中运行         |
| CLICE_OFFLINE_BUILD    | OFF    | 禁止在配置期间从网络下载                                       |

## 关于 LLVM

clice 调用 Clang API 解析 C++ 代码，因此必须链接 LLVM/Clang。由于 clice 使用 Clang 的私有头文件（发行版软件包通常不包含这些文件），因此无法直接使用系统提供的 LLVM 软件包。

可以通过以下两种方式满足此依赖：

1. 我们在 [clice-llvm](https://github.com/clice-io/clice-llvm/releases) 中发布所用 LLVM 版本的预编译二进制文件，供 CI 和发布版构建使用。构建时，CMake 默认会下载这些 LLVM 库。

> [!IMPORTANT]
>
> 对于 LLVM 的调试构建，我们会启用 Address Sanitizer；它依赖 compiler-rt，并且对编译器版本非常敏感。如果使用调试构建，请确保 clang 的 compiler-rt 版本与 `pixi.toml` 中定义的版本一致。

2. 自行构建 LLVM/Clang，使其与你的环境相匹配。如果默认的预编译二进制文件因 ABI 或库版本不匹配而无法使用，或者你需要自定义调试构建，请采用这种方式。我们提供了 `scripts/build-llvm.py`，用于构建所需的 LLVM 库；你也可以参阅 LLVM 官方指南[使用 CMake 构建 LLVM](https://llvm.org/docs/CMake.html)。
