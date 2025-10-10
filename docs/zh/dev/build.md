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

我们提供了完整的 Docker 开发容器解决方案，包含预配置的编译器、构建工具和所有必要依赖，彻底解决环境配置问题。

### 🚀 快速开始

#### 构建开发容器
```bash
# 构建默认容器（clang + latest 版本）
./docker/linux/build.sh

# 构建特定编译器和版本的容器
./docker/linux/build.sh --compiler gcc --version v1.2.3
```

#### 运行开发容器
```bash
# 运行默认容器
./docker/linux/run.sh

# 运行特定编译器容器
./docker/linux/run.sh --compiler clang
./docker/linux/run.sh --compiler gcc

# 运行特定版本容器
./docker/linux/run.sh --compiler clang --version v1.2.3
```

#### 容器管理
```bash
# 重置容器（删除现有容器）
./docker/linux/run.sh --reset

# 更新容器镜像（拉取最新版本）
./docker/linux/run.sh --update

# 重建容器镜像
./docker/linux/run.sh --rebuild
```

### 🏗️ 开发工作流程

#### 完整开发流程示例
```bash
# 1. 构建开发容器
./docker/linux/build.sh --compiler clang

# 2. 启动开发会话
./docker/linux/run.sh --compiler clang

# 3. 在容器内构建项目
cd /clice
mkdir build && cd build

# 使用 CMake 构建
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLLVM_INSTALL_PATH="/usr/local/llvm"
ninja

# 或使用 XMake 构建
xmake f --mode=debug --toolchain=clang
xmake build --all
```

### 📦 容器特性

#### 预装工具和环境
- **编译器**：GCC 14, Clang 20（来自官方 LLVM PPA）
- **构建系统**：CMake 3.28+, XMake 2.8+
- **开发工具**：完整的 C++ 开发栈，包括调试器、分析器等
- **LLVM 库**：预配置的 LLVM 20.x 开发库和头文件
- **Python 环境**：所有阶段都使用一致的 uv 和 Python 环境

#### 容器架构优势
- **多阶段构建**：通过并行执行优化构建时间和镜像大小
- **智能缓存**：APT 包、工具二进制文件和 Python 包的高效缓存机制
- **并行处理**：依赖下载和工具链构建同时运行
- **独立缓存命名空间**：每个阶段使用独立的缓存 ID，实现真正的并行执行
- **版本管理**：支持多版本并存，精确的版本控制

#### 自动化特性
- **依赖解析**：自动解析完整的依赖树
- **环境验证**：自动验证开发环境完整性
- **持久化**：容器状态在会话间保持持久
- **自动挂载**：项目目录自动挂载到 `/clice`

### 🎯 使用场景

#### 日常开发
```bash
# 启动开发环境
./docker/linux/run.sh

# 容器会自动：
# - 检查并启动现有容器，或创建新容器
# - 挂载项目目录到 /clice
# - 提供完整的开发环境
```

#### 多版本测试
```bash
# 测试不同编译器
./docker/linux/run.sh --compiler gcc
./docker/linux/run.sh --compiler clang

# 测试特定版本
./docker/linux/run.sh --version v1.0.0
./docker/linux/run.sh --version latest
```

### 📋 容器配置

#### 支持的参数
| 参数 | 描述 | 默认值 |
|------|------|--------|
| `--compiler` | 编译器类型 (gcc/clang) | `clang` |
| `--version` | 版本标签 | `latest` |
| `--reset` | 重置容器 | - |
| `--rebuild` | 强制重建镜像 | - |
| `--update` | 拉取最新镜像 | - |

#### 生成的镜像命名
- 格式：`clice-io/clice:linux-{compiler}-{version}`
- 示例：
  - `clice-io/clice:linux-clang-latest`
  - `clice-io/clice:linux-gcc-v1.2.3`

### 🔧 高级用法

#### 自定义命令执行
```bash
# 在容器中执行特定命令
./docker/linux/run.sh "cmake --version && xmake --version"

# 运行测试
./docker/linux/run.sh "cd /clice/build && ctest"

# 交互式调试
./docker/linux/run.sh "gdb ./build/clice"
```

### ⚡ 性能优化

#### 并行构建架构
容器系统在两个层面实现并行优化：

**Stage 间并行**：
- 工具链构建器和依赖下载器阶段同时执行
- Docker 构建引擎同时运行多个构建阶段
- 最大化构建资源利用率，减少总构建时间

**Stage 内并行**：
- 使用 `aria2c` 多连接并行下载
- APT 包批量并发下载
- 不同类型依赖（APT、工具、Python）并行获取
- 完整依赖树预解析，减少下载时依赖查找开销

#### 缓存独立性
每个构建阶段使用独立的缓存命名空间：
- `toolchain-builder-*` - 工具链构建缓存
- `dependencies-downloader-*` - 依赖下载缓存
- `packed-image-*` - 包创建缓存

确保真正的并行执行，避免缓存冲突。

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
