# Build from Source

clice builds with [Bazel](https://bazel.build) alone. Bazel downloads everything the build uses: the C++ toolchain and the prebuilt LLVM/Clang libraries of one [xclang](https://github.com/clice-io/xclang) release, clice-io's clang toolchain, and the other libraries clice links. The repository's pixi and npm set up the tools the tests and the docs need; building clice needs neither.

## Requirements

- Bazel, run through [bazelisk](https://github.com/bazelbuild/bazelisk), which fetches the release `.bazelversion` names.
- git and [Node.js](https://nodejs.org) on `PATH`: the build stamps clice with the version `git describe` gives, through a Node script.
- On Windows, a Bash, which [Git for Windows](https://gitforwindows.org) provides, and a short output root: Bazel's default one is too deep for Windows paths. Put it in `%USERPROFILE%\.bazelrc`:

  ```
  startup --output_user_root=C:/b
  ```

## Building

```shell
bazel build //:bin/clice
```

clice is `build/RelWithDebInfo/bin/bin/clice`, beside the resource directory it reads clang's headers from, `build/RelWithDebInfo/bin/lib/clang`.

The build types are configurations, each with a directory of its own, `build/<type>`:

| Configuration             | Effect                                                                             |
| ------------------------- | ---------------------------------------------------------------------------------- |
| `--config=RelWithDebInfo` | The default: optimized, with debug info                                            |
| `--config=Debug`          | Light optimization (`-O1`) with Address Sanitizer, on x86_64 Linux and arm64 macOS |

`--platforms=@xclang//platforms:<triple>` builds for another target: x86_64 Linux builds every other one, for example `--platforms=@xclang//platforms:aarch64-apple-darwin`.

The LLVM/Clang libraries hold ThinLTO bitcode, so every link of a program redoes their code generation, minutes per program. lld keeps what it generated in a cache, `/var/tmp/xclang-thinlto` (`C:/xclang-thinlto` on Windows), and later links take seconds.

`bazel build //:package //:symbols` builds the release archive and the symbol package, which `scripts/symbolize.py` reads: `clice.tar.gz` and `clice-symbol.tar.xz` in `build/<type>/bin` (`.zip` on Windows).

## Working on clice

`bazel run @compdb//:refresh -- --output_groups=+compilation_prerequisites_INTERNAL_` writes a `compile_commands.json` of clice's own sources to the repository root, for clice to work on its own code; the output group builds the generated headers the commands read.

The tests need the tools pixi installs. `bazel build //:dist` builds clice with the unit tests, then `pixi run test` runs the unit tests and the cases (`--config=Debug` and `pixi run test Debug` for the Debug build); [`tests/README.md`](https://github.com/clice-io/clice/blob/main/tests/README.md) says how a case is written.

## Another LLVM Build

clice links the LLVM/Clang libraries of the exact version it is written for, not the system's. A build of one's own replaces the release's with `--repo_env=XCLANG_LIBCLANG_ROOT=<directory>` (`XCLANG_LIBCLANG_ASAN_ROOT` for the ASan one); it has to be built the way xclang builds it, by xclang's `scripts/toolchain.ts`.
