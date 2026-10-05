# Build from Source

clice depends on C++23 features and requires a modern C++ toolchain. We also need to link against LLVM/Clang to parse ASTs. Both come from [xclang](https://github.com/clice-io/xclang), clice-io's clang toolchain: [Bazel](https://bazel.build) builds clice with xclang's toolchain and links the prebuilt LLVM/Clang libraries of the same xclang release, both downloaded by Bazel itself. The two must match: the libraries hold ThinLTO bitcode, which only the toolchain of that release reads.

To simplify setup and keep builds reproducible, we **strongly recommend** [pixi](https://pixi.prefix.dev/latest) to manage the development environment. Dependency versions are pinned in `pixi.toml`; Bazel's are in `MODULE.bazel`.

## Quick Start

Install pixi following the [official guide](https://pixi.prefix.dev/latest/installation).

We ship several tasks; the commands below build and run tests:

```shell
# build (default RelWithDebInfo) into build/RelWithDebInfo
pixi run build

# unit + integration + smoke + snap tests
pixi run test
```

For finer-grained tasks (first argument sets the build type):

```shell
pixi run build Debug
pixi run unit-test Debug
pixi run integration-test Debug
pixi run smoke-test Debug
pixi run snap-test Debug
```

`pixi run build` builds with Bazel and lays the programs out in `build/<type>/bin`, next to the resource directory clice reads clang's headers from, `build/<type>/lib/clang`; the tests and the editors run them from there.

> [!TIP]
> Run `pixi shell` to enter a shell with all env vars configured, and `npx bazel` there for Bazel itself.

## Bazel

Bazel is run through [bazelisk](https://github.com/bazelbuild/bazelisk), an npm package of the repository (`npm install` installs it), which fetches the Bazel release `.bazelversion` names:

```shell
npx bazel build //:clice //:unit_tests
```

The build types are configurations of `.bazelrc`:

| Configuration             | Effect                                                                                                                                                 |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `--config=RelWithDebInfo` | The default: optimized, with debug info                                                                                                                |
| `--config=Debug`          | Unoptimized, with Address Sanitizer; on Windows without it                                                                                             |
| `--config=release`        | Links the programs with the LLVM/Clang libraries' ThinLTO bitcode, as releases ship clice; other builds link them compiled to native code, much faster |

Options after `--` reach Bazel through `pixi run build`, for example `pixi run build RelWithDebInfo -- --config=release`.

`pixi run compile-commands` writes a `compile_commands.json` of clice's own sources to the repository root, for clice to work on its own code.

On Windows, Bazel's default output root is too deep for Windows paths; put a short one in `%USERPROFILE%\.bazelrc`:

```
startup --output_user_root=C:/b
```

Bazel also needs a Bash on Windows, which [Git for Windows](https://gitforwindows.org) provides.

## About LLVM

clice calls Clang APIs to parse C++ code, so it must link against LLVM/Clang. Because clice uses Clang's private headers (usually absent from distro packages), the system LLVM package cannot be used directly.

Every [xclang](https://github.com/clice-io/xclang/releases) release publishes prebuilt LLVM/Clang libraries (the `libclang-*` archives) for all six targets, built by that release's toolchain, and its Bazel module makes them repositories Bazel downloads with the toolchain.

> [!IMPORTANT]
>
> Debug builds enable Address Sanitizer and link the ASan-instrumented libraries xclang publishes for x86_64 Linux and arm64 macOS; arm64 Linux and x86_64 macOS have none, and no Debug build. Debug builds for Windows link the release libraries, without Address Sanitizer.

A build of LLVM/Clang of one's own replaces the release's with `--repo_env=XCLANG_LIBCLANG_ROOT=<directory>` (`XCLANG_LIBCLANG_ASAN_ROOT` for the ASan one); it has to be built the way xclang builds it, by xclang's `scripts/toolchain.ts`; see [xclang](https://github.com/clice-io/xclang).
