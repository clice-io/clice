# Build from Source

clice depends on C++23 features and requires a modern C++ toolchain. We also need to link against LLVM/Clang to parse ASTs. Both come from [xclang](https://github.com/clice-io/xclang), clice-io's clang toolchain: pixi installs its compiler, and the default configuration downloads the prebuilt LLVM/Clang libraries of the same xclang release. The two must match: the libraries hold ThinLTO bitcode, which only the compiler of that release reads.

To simplify setup and keep builds reproducible, we **strongly recommend** [pixi](https://pixi.prefix.dev/latest) to manage the development environment. Dependency versions are pinned in `pixi.toml`.

If you prefer not to use pixi, see [Manual Build](#manual-build) below.

## Quick Start

Install pixi following the [official guide](https://pixi.prefix.dev/latest/installation).

We ship several tasks; the commands below configure, build, and run tests:

```shell
# configure && build (default RelWithDebInfo)
pixi run build

# unit + integration + smoke + snap tests
pixi run test
```

For finer-grained tasks (first argument sets the build type):

```shell
pixi run cmake-config Debug
pixi run cmake-build Debug
pixi run unit-test Debug
pixi run integration-test Debug
pixi run smoke-test Debug
pixi run snap-test Debug
```

> [!TIP]
> If you want to develop directly with `cmake`, `ninja`, `clang++`, etc., run `pixi shell` to enter a shell with all env vars configured.

## Manual Build

If you plan to build manually, first ensure your toolchain matches the versions defined in `pixi.toml`.

> Compatibility: clice itself does not rely on compiler-specific extensions, but the LLVM/Clang libraries it links hold ThinLTO bitcode, so the compiler must be the xclang release pinned in `pixi.toml`; `cmake/llvm.cmake` checks this at configure time. Please open an issue or PR if you hit problems.

### CMake

```shell
cmake -B build/RelWithDebInfo -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake \
    -DCLICE_ENABLE_TEST=ON

cmake --build build/RelWithDebInfo
```

> Note: `CMAKE_TOOLCHAIN_FILE` is optional. If your toolchain exactly matches ours, you can use the predefined `cmake/toolchain.cmake`; otherwise remove that flag.

### CMake Options

| Option                 | Default | Effect                                                         |
| ---------------------- | ------- | -------------------------------------------------------------- |
| LLVM_INSTALL_PATH      | ""      | Build clice with LLVM from a custom path                       |
| CLICE_ENABLE_TEST      | OFF     | Build unit tests and benchmarks infrastructure                 |
| CLICE_ENABLE_BENCHMARK | OFF     | Build benchmarks                                               |
| CLICE_ENABLE_LTO       | OFF     | Enable ThinLTO for all targets                                 |
| CLICE_CI_ENVIRONMENT   | OFF     | Enable `CLICE_CI_ENVIRONMENT` macro; some tests only run in CI |
| CLICE_OFFLINE_BUILD    | OFF     | Disable network downloads during configuration                 |

## About LLVM

clice calls Clang APIs to parse C++ code, so it must link against LLVM/Clang. Because clice uses Clang's private headers (usually absent from distro packages), the system LLVM package cannot be used directly.

Two ways to satisfy this dependency:

1. Every [xclang](https://github.com/clice-io/xclang/releases) release publishes prebuilt LLVM/Clang libraries (the `libclang-*` archives) for all six targets, built by that release's toolchain. During builds, cmake downloads the archive of the target by default.

> [!IMPORTANT]
>
> Debug builds for x86_64 Linux and arm64 macOS enable Address Sanitizer and link the ASan-instrumented libraries xclang publishes for these two targets. Debug builds for the other targets link the release libraries, without Address Sanitizer.

2. Build LLVM/Clang yourself and pass the install directory as `LLVM_INSTALL_PATH`. `cmake/llvm.cmake` checks the install against the manifest xclang writes, `lib/cmake/xclang/libclang.cmake`, so the way to build one is xclang's `scripts/toolchain.ts`; see [xclang](https://github.com/clice-io/xclang).
