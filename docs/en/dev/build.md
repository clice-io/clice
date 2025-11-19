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

You can also refer to LLVM's official build tutorial: [Building LLVM with CMake](https://llvm.org/docs/CMake.html).
