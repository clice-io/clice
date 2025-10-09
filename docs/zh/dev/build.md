# Build from Source

## Supported Platforms

- Windows
- Linux
- MacOS

## Prerequisite

本小节介绍编译 clice 的前置依赖。

### Toolchain

- clang >= 19
- c++23 compitable standard library
  - MSVC STL >= 19.44(VS 2022 17.4)
  - GCC libstdc++ >= 14
  - Clang libc++ >= 20

clice 使用 C++23 作为语言标准 ，请确保有可用的 clang 19 以及以上的编译器，以及兼容 C++23 的标准库。

> clice 暂时只能使用 clang 编译，在未来我们会改进这一点，使其能使用 gcc 和 msvc 编译。

### LLVM Libs

- 20.1.5 <= llvm libs < 21

由于 C++ 的语法太过复杂，自己编写一个新的 parser 是不现实的。clice 调用 clang 的 API 来 parse C++ 源文件获取 AST，这意味它需要链接 llvm/clang libs。另外由于 clice 使用了 clang 的私有头文件，这些私有头文件在 llvm 发布的 binary release 中是没有的，所以不能直接使用系统的 llvm package。

如果你能找到系统的 llvm package 对应的 llvm commit，将该 commit 下的如下三个文件

- `clang/lib/Sema/CoroutineStmtBuilder.h`
- `clang/lib/Sema/TypeLocBuilder.h`
- `clang/lib/Sema/TreeTransform.h`

拷贝到 `LLVM_INSTALL_PATH/include/clang/Sema/` 中即可。

除了这种方法以外，还有两种办法获取 clice 所需的 llvm libs：

1. 使用我们提供的预编译版本

```bash
# .github/workflows/cmake.yml

# Linux precompiled binary require glibc 2.35 (build on ubuntu 22.04)
$ mkdir -p ./.llvm
$ curl -L "https://github.com/clice-io/llvm-binary/releases/download/20.1.5/x86_64-linux-gnu-release.tar.xz" | tar -xJ -C ./.llvm

# MacOS precompiled binary require macos15+
$ mkdir -p ./.llvm
$ curl -L "https://github.com/clice-io/llvm-binary/releases/download/20.1.5/arm64-macosx-apple-release.tar.xz" | tar -xJ -C ./.llvm

# Windows precompiled binary only MD runtime support
$ curl -O -L "https://github.com/clice-io/llvm-binary/releases/download/20.1.5/x64-windows-msvc-release.7z"
$ 7z x x64-windows-msvc-release.7z "-o.llvm"
```

> [!IMPORTANT]
>
> 对于 debug 版本的 llvm libs，构建的时候我们开启了 address sanitizer，而 address sanitizer 依赖于 compiler rt，它对编译器版本十分敏感。所以如果使用 debug 版本，请确保你的 clang 的 compiler rt 版本和我们构建的时候**严格一致**。
>
> - Windows 暂时无 debug 构建的 llvm libs，因为它不支持将 clang 构建为动态库，相关的进展可以在 [这里](https://discourse.llvm.org/t/llvm-is-buildable-as-a-windows-dll/87748) 找到
> - Linux 使用 clang20
> - MacOS 使用 homebrew llvm@20，一定不要使用 apple clang

2. 自己从头编译 llvm/clang

这是最推荐的方式，可以保证环境一致性，避免因为 ABI 不一致而导致的崩溃问题。我们提供了一个脚本，用于构建 clice 所需要的 llvm libs：[build-llvm-libs.py](https://github.com/clice-io/clice/blob/main/scripts/build-llvm-libs.py)。

```bash
$ cd llvm-project
$ python3 <clice>/scripts/build-llvm-libs.py debug
```

也可以参考 llvm 的官方构建教程 [Building LLVM with CMake](https://llvm.org/docs/CMake.html)。

### GCC Toolchain

clice 要求 GCC libstdc++ >= 14。以下命令使用不同的 GCC 工具链并静态链接其 libstdc++：

```bash
cmake .. -DCMAKE_C_FLAGS="--gcc-toolchain=/usr/local/gcc-14.3.0/" \
         -DCMAKE_CXX_FLAGS="--gcc-toolchain=/usr/local/gcc-14.3.0/" \
         -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++"
```

## Building

在处理好前置依赖之后，可以开始构建 clice 了，我们提供 cmake/xmake 两种构建方式。

### CMake

下面是 clice 支持的 cmake 参数

- `LLVM_INSTALL_PATH` 用于指定 llvm libs 的安装路径
- `CLICE_ENABLE_TEST` 是否构建 clice 的单元测试

例如

```bach
$ cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLLVM_INSTALL_PATH="./.llvm" -DCLICE_ENABLE_TEST=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
$ cmake --build build
```

### Xmake

使用如下的命令即可构建 clice

```bash
$ xmake f -c --dev=true --mode=debug --toolchain=clang --llvm="./.llvm" --enable_test=true
$ xmake build --all
```

> --llvm 是可选的，如果不指定的话，xmake 会自动下载我们编译好的预编译二进制

## Dev Container

我们提供了 docker 镜像作为预装环境解决方案，可以有效地解决环境配置问题，可通过下列命令使用（不限脚本调用路径，可以直接运行 ./build.sh）：

```bash
# construct container
docker/linux/build.sh
# run clang container
docker/linux/run.sh --compiler clang
# run gcc container
docker/linux/run.sh --compiler gcc
# reset container(delete exist container and reset)
docker/linux/run.sh --reset
```

> [!NOTE]
> 当前该功能仍处于 Preview 阶段，仅支持 Linux，后续会提供 Windows 平台版本，并可能存在功能改动

## Building Docker Image

使用以下命令构建 docker 镜像：

```bash
$ docker build -t clice .
```

运行 docker 镜像：

```bash
$ docker run --rm -it clice --help
OVERVIEW: clice is a new generation of language server for C/C++
...
```

docker 镜像的目录结构如下：

```
/opt/clice
├── bin
│   ├── clice -> /usr/local/bin/clice
├── include
├── lib
├── LICENSE
├── README.md
```

提示：可以使用以下命令进入 clice 容器：

```bash
$ docker run --rm -it --entrypoint bash clice
