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

clice uses C++23 as its language standard. Please ensure you have a clang 20 (or higher) compiler and a C++23 compatible standard library available. clice depends on lld as its linker. Please ensure your clang toolchain can find it (clang distributions usually bundle lld, or you may need to install the lld-20 package separately).

> clice is currently only guaranteed to compile with clang (as ensured by CI testing). We do our best to maintain compatibility with gcc and msvc, but we do not add corresponding tests in CI. Contributions are welcome if you encounter any issues.

## CMake

Use the following commands to build clice

```shell
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

Optional build options:

| Option               | Default | Description                                                                                                                    |
| :------------------- | :------ | :----------------------------------------------------------------------------------------------------------------------------- |
| LLVM_INSTALL_PATH    | ""      | Build clice using llvm libs from a custom path                                                                                 |
| CLICE_ENABLE_TEST    | OFF     | Whether to build clice's unit tests                                                                                            |
| CLICE_USE_LIBCXX     | OFF     | Whether to build clice with libc++ (adds `-std=libc++`). If enabled, ensure that the llvm libs were also compiled with libc++. |
| CLICE_CI_ENVIRONMENT | OFF     | Whether to enable the `CLICE_CI_ENVIRONMENT` macro. Some tests only run in a CI environment.                                   |

## XMake

Use the following commands to build clice

```bash
xmake f -c --mode=releasedbg --toolchain=clang
xmake build --all
```

Optional build options:

| Option        | Default | Description                                    |
| :------------ | :------ | :--------------------------------------------- |
| --llvm        | ""      | Build clice using llvm libs from a custom path |
| --enable_test | false   | Whether to build clice's unit tests            |
| --ci          | false   | Whether to enable `CLICE_CI_ENVIRONMENT`       |

## A Note on LLVM Libs

Due to the complexity of C++ syntax, writing a new parser from scratch is unrealistic. clice calls clang's APIs to parse C++ source files and obtain the AST, which means it needs to link against llvm/clang libs. Because clice uses clang's private headers, which are not included in the binary releases published by LLVM, you cannot use the system's llvm package directly.

1. We publish pre-compiled binaries for the LLVM version we use on [clice-llvm](https://github.com/clice-io/clice-llvm/releases), which are used for CI or release builds. By default, cmake and xmake will download and use the llvm libs from here during the build.

> [!IMPORTANT]
>
> For debug builds of llvm libs, we enable address sanitizer. Address sanitizer depends on compiler-rt, which is highly sensitive to the compiler version.
>
> Therefore, if you use a debug build, please ensure your clang's compiler-rt version is **strictly identical** to the one used in our build.
>
> - Windows does not currently have debug builds for llvm libs, as it does not support building clang as a dynamic library. Related progress is tracked [here](https://github.com/clice-io/clice/issues/42).
> - Linux uses clang20
> - MacOS uses homebrew llvm@20. **Do not use apple clang**.
>
> You can refer to the [cmake](https://github.com/clice-io/clice/blob/main/.github/workflows/cmake.yml) and [xmake](https://github.com/clice-io/clice/blob/main/.github/workflows/xmake.yml) files in our CI as a reference, as they maintain an environment strictly consistent with the pre-compiled llvm libs.

2. Build llvm/clang yourself to match your current environment. If the default pre-compiled binaries (Method 1) fail to run on your system due to ABI or library version (e.g., glibc) incompatibility, or if you need a custom Debug build, we recommend you use this method to compile llvm libs from scratch. We provide a script to build the llvm libs required by clice: [build-llvm-libs.py](https://github.com/clice-io/clice/blob/main/scripts/build-llvm-libs.py).

```bash
cd llvm-project
python3 <clice>/scripts/build-llvm-libs.py debug
```

You can also refer to llvm's official build tutorial: [Building LLVM with CMake](https://llvm.org/docs/CMake.html).

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
```

> [!NOTE]
> This feature is currently in a preview stage and only supports Linux. Windows support will be provided in the future, and the functionality may be subject to change.
>>>>>>> 58bcd0e (feat: implement advanced multi-stage dev container architecture)
