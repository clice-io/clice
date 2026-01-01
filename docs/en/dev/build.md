# Build from Source

clice depends on C++23 features and requires a modern C++ toolchain. We also need to link against LLVM/Clang to parse ASTs. To speed up builds, the default configuration downloads our published [clice-llvm](https://github.com/clice-io/clice-llvm) prebuilt package. This assumes your local environment matches the prebuilt environment closely (especially when enabling Address Sanitizer or LTO).

To simplify setup and keep builds reproducible, we **strongly recommend** [pixi](https://pixi.prefix.dev/latest) to manage the development environment. Dependency versions are pinned in `pixi.toml`.

If you prefer not to use pixi, see [Manual Build](#manual-build) below.

## Quick Start

Install pixi following the [official guide](https://pixi.prefix.dev/latest/installation).

We ship several tasks; the commands below configure, build, and run tests:

```shell
# configure && build (default RelWithDebInfo)
pixi run build

# unit && integration
pixi run test
```

For finer-grained tasks (first argument sets the build type):

```shell
pixi run cmake-config Debug
pixi run cmake-build Debug
pixi run unit-test Debug
pixi run integration-test Debug
```

> [!TIP]
> If you want to develop directly with `cmake`, `ninja`, `clang++`, etc., run `pixi shell` to enter a shell with all env vars configured.

### XMake

We also support building with XMake:

```shell
# config & build (default releasedbg)
pixi run xmake

# unit & integration
pixi run xmake-test
```

## Manual Build

If you plan to build manually, first ensure your toolchain matches the versions defined in `pixi.toml`.

> Compatibility: In theory clice does not rely on compiler-specific extensions, so mainstream compilers (GCC/Clang/MSVC) should work. However, CI only guarantees specific versions of Clang. Other compilers or versions are supported on a **best-effort** basis. Please open an issue or PR if you hit problems.

### CMake

```shell
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake \
    -DCLICE_ENABLE_TEST=ON
```

> Note: `CMAKE_TOOLCHAIN_FILE` is optional. If your toolchain exactly matches ours, you can use the predefined `cmake/toolchain.cmake`; otherwise remove that flag.

Optional build options:

| Option               | Default | Effect                                                                                                    |
| -------------------- | ------- | --------------------------------------------------------------------------------------------------------- |
| LLVM_INSTALL_PATH    | ""      | Build clice with LLVM from a custom path                                                                  |
| CLICE_ENABLE_TEST    | OFF     | Build clice unit tests                                                                                    |
| CLICE_USE_LIBCXX     | OFF     | Build clice with libc++ (adds `-std=libc++`); if enabled, ensure the LLVM libs are also built with libc++ |
| CLICE_CI_ENVIRONMENT | OFF     | Enable the `CLICE_CI_ENVIRONMENT` macro; some tests only run in CI                                        |

### XMake

Build clice with:

```bash
xmake f -c --mode=releasedbg --toolchain=clang
xmake build --all
```

Optional build options:

| Option        | Default | Effect                                   |
| ------------- | ------- | ---------------------------------------- |
| --llvm        | ""      | Build clice with LLVM from a custom path |
| --enable_test | false   | Build clice unit tests                   |
| --ci          | false   | Enable `CLICE_CI_ENVIRONMENT`            |

## About LLVM

clice calls Clang APIs to parse C++ code, so it must link against LLVM/Clang. Because clice uses Clang's private headers (usually absent from distro packages), the system LLVM package cannot be used directly.

Two ways to satisfy this dependency:

1. We publish prebuilt binaries of the LLVM version we use at [clice-llvm](https://github.com/clice-io/clice-llvm/releases) for CI and release builds. During builds, cmake and xmake download these LLVM libs by default.

> [!IMPORTANT]
>
> For debug LLVM builds, we enable address sanitizer, which depends on compiler-rt and is very sensitive to compiler version. If you use a debug build, ensure your clang compiler-rt version matches the one defined in `pixi.toml`.

2. Build LLVM/Clang yourself to match your environment. If the default prebuilt binaries fail due to ABI or library version mismatches, or you need a custom debug build, use this approach. We provide `scripts/build-llvm.py` to build the required LLVM libs, or refer to LLVM's official guide [Building LLVM with CMake](https://llvm.org/docs/CMake.html).
