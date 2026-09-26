# 从源码构建

clice 依赖 C++23 特性，需要使用现代 C++ 工具链。同时还需要链接 LLVM/Clang 以解析 AST。两者都来自 clice-io 的 clang 工具链 [xclang](https://github.com/clice-io/xclang)：pixi 安装它的编译器，默认配置则下载同一 xclang 版本预编译的 LLVM/Clang 库。两者必须一致：这些库里是 ThinLTO bitcode，只有该版本的编译器能读取。

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

> 兼容性说明：clice 本身不依赖任何编译器特有的扩展，但它链接的 LLVM/Clang 库里是 ThinLTO bitcode，因此编译器必须是 `pixi.toml` 中固定的 xclang 版本；`cmake/llvm.cmake` 会在配置时检查这一点。遇到问题欢迎提交 issue 或 PR。

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

| 选项                   | 默认值 | 作用                                                   |
| ---------------------- | ------ | ------------------------------------------------------ |
| LLVM_INSTALL_PATH      | ""     | 使用自定义路径中的 LLVM 构建 clice                     |
| CLICE_ENABLE_TEST      | OFF    | 构建单元测试和基准测试基础设施                         |
| CLICE_ENABLE_BENCHMARK | OFF    | 构建基准测试                                           |
| CLICE_ENABLE_LTO       | OFF    | 为所有目标启用 ThinLTO                                 |
| CLICE_CI_ENVIRONMENT   | OFF    | 启用 `CLICE_CI_ENVIRONMENT` 宏；部分测试仅在 CI 中运行 |
| CLICE_OFFLINE_BUILD    | OFF    | 禁止在配置期间从网络下载                               |

## 关于 LLVM

clice 调用 Clang API 解析 C++ 代码，因此必须链接 LLVM/Clang。由于 clice 使用 Clang 的私有头文件（发行版软件包通常不包含这些文件），因此无法直接使用系统提供的 LLVM 软件包。

可以通过以下两种方式满足此依赖：

1. 每个 [xclang](https://github.com/clice-io/xclang/releases) 版本都会为全部六个目标发布预编译的 LLVM/Clang 库（`libclang-*` 压缩包），由该版本自己的工具链构建。构建时，CMake 默认会下载对应目标的压缩包。

> [!IMPORTANT]
>
> 在 x86_64 Linux 和 arm64 macOS 上，调试构建会启用 Address Sanitizer，并链接 xclang 为这两个目标发布的 ASan 插桩库。其他目标的调试构建链接发布版的库，不启用 Address Sanitizer。

2. 自行构建 LLVM/Clang，并通过 `LLVM_INSTALL_PATH` 传入安装目录。`cmake/llvm.cmake` 会用 xclang 写入的清单 `lib/cmake/xclang/libclang.cmake` 检查该安装，因此构建方法是使用 xclang 的 `scripts/toolchain.ts`；参见 [xclang](https://github.com/clice-io/xclang)。
