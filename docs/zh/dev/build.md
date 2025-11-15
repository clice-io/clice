# Build from Source

## Supported Platforms

- Windows
- Linux
- MacOS

## Prerequisite

- cmake/xmake
- clang, lld >= 20
- c++23 **compatible** standard library
  - MSVC STL >= 19.44(VS 2022 17.4)
  - GCC libstdc++ >= 14
  - Clang libc++ >= 20

clice 使用 C++23 作为语言标准，请确保有可用的 clang 20 以及以上的编译器，以及兼容 C++23 的标准库。clice 依赖 lld 作为链接器。请确保你的 clang 工具链可以找到它（通常 clang 发行版会自带 lld，或者你需要单独安装 lld-20 包）。

> clice 目前只保证能使用 clang 编译（CI 测试保证）。对于 gcc 和 msvc 的兼容，我们尽力而为，但不会在 CI 中添加对应的测试。如果遇到任何问题，欢迎贡献。

## CMake

使用如下的命令构建 clice

```shell
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

可选的构建选项：

| 选项                 | 默认值 | 效果                                                                                                  |
| -------------------- | ------ | ----------------------------------------------------------------------------------------------------- |
| LLVM_INSTALL_PATH    | ""     | 使用自定义路径的 llvm libs 来构建 clice                                                               |
| CLICE_ENABLE_TEST    | OFF    | 是否构建 clice 的单元测试                                                                             |
| CLICE_USE_LIBCXX     | OFF    | 是否使用 libc++ 来构建 clice（添加 `-std=libc++`），如果开启，请确保 llvm libs 也是使用 libc++ 编译的 |
| CLICE_CI_ENVIRONMENT | OFF    | 是否打开 `CLICE_CI_ENVIRONMENT` 这个宏，有些测试在 CI 环境才会执行                                    |

## XMake

使用如下的命令即可构建 clice

```bash
xmake f -c --mode=releasedbg --toolchain=clang
xmake build --all
```

可选的构建选项：

| 选项          | 默认值 | 效果                                    |
| ------------- | ------ | --------------------------------------- |
| --llvm        | ""     | 使用自定义路径的 llvm libs 来构建 clice |
| --enable_test | false  | 是否构建 clice 的单元测试               |
| --ci          | false  | 是否打开 `CLICE_CI_ENVIRONMENT`         |


## A Note on LLVM Libs

由于 C++ 的语法太过复杂，自己编写一个新的 parser 是不现实的。clice 调用 clang 的 API 来 parse C++ 源文件获取 AST，这意味它需要链接 llvm/clang libs。由于 clice 使用了 clang 的私有头文件，这些私有头文件在 llvm 发布的 binary release 中是没有的，所以不能直接使用系统的 llvm package。

1. 我们在 [clice-llvm](https://github.com/clice-io/clice-llvm/releases) 上会发布使用的 llvm 版本的预编译二进制，用于 CI 或者 release 构建。在构建时 cmake 和 xmake 默认会从此处下载 llvm libs 然后使用，

> [!IMPORTANT]
>
> 对于 debug 版本的 llvm libs，构建的时候我们开启了 address sanitizer，而 address sanitizer 依赖于 compiler rt，它对编译器版本十分敏感。所以如果使用 debug 版本，请确保你的 clang 的 compiler rt 版本和我们构建的时候**严格一致**。
>
> - Windows 暂时没有 debug 构建的 llvm libs，因为它不支持将 clang 构建为动态库，相关的进展在 [这里](https://github.com/clice-io/clice/issues/42) 跟踪
> - Linux 使用 clang20
> - MacOS 使用 homebrew llvm@20，**不要使用 apple clang**
>
> 可以参考 CI 中的 [cmake](https://github.com/clice-io/clice/blob/main/.github/workflows/cmake.yml) 和 [xmake](https://github.com/clice-io/clice/blob/main/.github/workflows/xmake.yml) 文件作为参考，它们与预编译 llvm libs 的环境保持严格一致。

2. 自己重新一个与当前环境一致的 llvm/clang。如果默认的预编译二进制文件（方法 1）在你的系统上因 ABI 或库版本（如 glibc）不兼容而运行失败，或者你需要一个自定义的 Debug 版本，那么我们推荐你使用此方法从头编译 llvm libs。我们提供了一个脚本，用于构建 clice 所需要的 llvm libs：[build-llvm-libs.py](https://github.com/clice-io/clice/blob/main/scripts/build-llvm-libs.py)。

```bash
cd llvm-project
python3 <clice>/scripts/build-llvm-libs.py debug
```

也可以参考 llvm 的官方构建教程 [Building LLVM with CMake](https://llvm.org/docs/CMake.html)。
