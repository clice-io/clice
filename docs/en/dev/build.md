# Build from Source

## Supported Platforms

- Windows
- Linux
- MacOS

## Prerequisites

This section introduces the prerequisites for compiling clice.

### Toolchain

- clang >= 19
- c++23 compatible standard library
  - MSVC STL >= 19.44(VS 2022 17.4)
  - GCC libstdc++ >= 14
  - Clang libc++ >= 20

clice uses C++23 as the language standard. Please ensure you have an available clang 19 or above compiler, as well as a standard library compatible with C++23.

> clice can currently only be compiled with clang. In the future, we will improve this to allow compilation with gcc and msvc.

### LLVM Libs

- 20.1.5 <= llvm libs < 21

Due to the complexity of C++ syntax, writing a new parser from scratch is unrealistic. clice calls clang's API to parse C++ source files and obtain AST, which means it needs to link llvm/clang libs. Additionally, since clice uses clang's private headers, these private headers are not available in llvm's binary release, so you cannot directly use the system's llvm package.

If you can find the llvm commit corresponding to your system's llvm package, copy the following three files from that commit:

- `clang/lib/Sema/CoroutineStmtBuilder.h`
- `clang/lib/Sema/TypeLocBuilder.h`
- `clang/lib/Sema/TreeTransform.h`

Copy them to `LLVM_INSTALL_PATH/include/clang/Sema/`.

Besides this method, there are two other ways to obtain the llvm libs required by clice:

1. Use our precompiled version

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
> For debug versions of llvm libs, we enabled address sanitizer during build, and address sanitizer depends on compiler rt, which is very sensitive to compiler versions. So if using debug versions, please ensure your clang's compiler rt version is **strictly consistent** with what we used during build.
>
> - Windows currently has no debug build of llvm libs because it doesn't support building clang as a dynamic library. Related progress can be found [here](https://discourse.llvm.org/t/llvm-is-buildable-as-a-windows-dll/87748)
> - Linux uses clang20
> - MacOS uses homebrew llvm@20, definitely don't use apple clang

2. Compile llvm/clang from scratch

This is the most recommended approach, ensuring environment consistency and avoiding crash issues caused by ABI inconsistencies. We provide a script for building the llvm libs required by clice: [build-llvm-libs.py](https://github.com/clice-io/clice/blob/main/scripts/build-llvm-libs.py).

```bash
$ cd llvm-project
$ python3 <clice>/scripts/build-llvm-libs.py debug
```

You can also refer to llvm's official build tutorial [Building LLVM with CMake](https://llvm.org/docs/CMake.html).

### GCC Toolchain

clice requires GCC libstdc++ >= 14. You could use a different GCC toolchain and also link statically against its libstdc++:

```bash
cmake .. -DCMAKE_C_FLAGS="--gcc-toolchain=/usr/local/gcc-14.3.0/" \
         -DCMAKE_CXX_FLAGS="--gcc-toolchain=/usr/local/gcc-14.3.0/" \
         -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++"
```

## Building

After handling the prerequisites, you can start building clice. We provide two build methods: cmake/xmake.

### CMake

Below are the cmake parameters supported by clice:

- `LLVM_INSTALL_PATH` specifies the installation path of llvm libs
- `CLICE_ENABLE_TEST` whether to build clice's unit tests

For example:

```bash
$ cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLLVM_INSTALL_PATH="./.llvm" -DCLICE_ENABLE_TEST=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
$ cmake --build build
```

### Xmake

Use the following command to build clice:

```bash
$ xmake f -c --dev=true --mode=debug --toolchain=clang --llvm="./.llvm" --enable_test=true
$ xmake build --all
```

> --llvm is optional. If not specified, xmake will automatically download our precompiled binary

## Dev Container

We provide a complete Docker development container solution with pre-configured compilers, build tools, and all necessary dependencies to completely solve environment configuration issues.

### 🚀 Quick Start

#### Run Development Container
```bash
# Run default container
./docker/linux/run.sh

# Run container with specific compiler
./docker/linux/run.sh --compiler gcc

# Run container with specific version
./docker/linux/run.sh --version v1.2.3
```

#### Container Management
```bash
# Reset container (remove and recreate)
./docker/linux/run.sh --reset

# Update container image (pull latest version)
./docker/linux/run.sh --update
```

### 🏗️ Development Workflow

#### Complete Development Flow Example
```bash
# 1. Start development session
./docker/linux/run.sh --compiler clang

# 2. Build project inside container (project directory auto-mounted to /clice)
cd /clice
mkdir build && cd build

# Build with CMake
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLLVM_INSTALL_PATH="/usr/local/llvm"
ninja

# Or build with XMake
xmake f --mode=debug --toolchain=clang
xmake build --all
```

### 📦 Container Features

#### Pre-installed Tools and Environment
- **Compilers**: GCC 14, Clang 20 (from official LLVM PPA)
- **Build Systems**: CMake 3.28+, XMake 2.8+
- **Development Tools**: Complete C++ development stack including debuggers, profilers, etc.
- **LLVM Libraries**: Pre-configured LLVM 20.x development libraries and headers
- **Python Environment**: Consistent Python environment managed by uv

#### Automation Features
- **Environment Isolation**: Independent containers per compiler and version
- **Persistence**: Container state persists across sessions
- **Auto-mount**: Project directory auto-mounted to `/clice`
- **Version Awareness**: Support creating dev environment from existing release images

### 🎯 Use Cases

#### Daily Development
```bash
# Start development environment (auto-build if image doesn't exist)
./docker/linux/run.sh

# Container will automatically:
# - Check and start existing container, or create new one
# - Mount project directory to /clice
# - Provide complete development environment
```

#### Multi-compiler Testing
```bash
# Test different compilers
./docker/linux/run.sh --compiler gcc
./docker/linux/run.sh --compiler clang

# Each compiler has independent container and environment
```

#### Version Management
```bash
# Use specific version
./docker/linux/run.sh --version v1.0.0

# Update to latest version (can be used with --version, but not effective for released versions as their images cannot be updated)
./docker/linux/run.sh --update
```

### 📋 Detailed Parameters

#### run.sh Parameters
| Parameter | Description | Default |
|-----------|-------------|---------|
| `--compiler <gcc\|clang>` | Compiler type | `clang` |
| `--version <version>` | Version tag | `latest` |
| `--reset` | Remove and recreate container | - |
| `--update` | Pull latest image and update | - |

#### Generated Image Naming Convention
- **Release image**: `clice-io/clice:linux-{compiler}-{version}`
- **Development image**: `clice-io/clice:linux-{compiler}-{version}-expanded`
- Examples:
  - `clice-io/clice:linux-clang-latest`
  - `clice-io/clice:linux-clang-latest-expanded`
  - `clice-io/clice:linux-gcc-v1.2.3`

### 🔧 Advanced Usage

#### Execute Custom Commands
```bash
# Execute specific command in container (use -- separator)
./docker/linux/run.sh -- cmake --version

# Execute multiple commands
./docker/linux/run.sh -- "cd /clice/build && cmake .."
```

#### Container Lifecycle Management
```bash
# Complete cleanup and rebuild
./docker/linux/run.sh --reset

# Update to latest image
./docker/linux/run.sh --update

# Check container status
docker ps -a | grep clice_dev
docker images | grep clice-io/clice
```

#### Container Persistence
- Container name: `clice_dev-linux-{compiler}-{version}`
- Working directory: `/clice` (mounted to host project directory)
- Container persists across sessions, all installed tools and configs are retained. Use `--reset` to remove the created container.

## Building Docker Image

Regular users only need to pull Docker images, not build from source.

Clice contributors can build Docker images from source. For detailed architecture documentation, see [dev-container-architecture.md](./dev-container-architecture.md).
