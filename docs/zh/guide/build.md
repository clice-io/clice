# 从源码构建

clice 仅用 [Bazel](https://bazel.build) 即可构建。构建所需的一切均由 Bazel 自行下载：clice-io 的 clang 工具链 [xclang](https://github.com/clice-io/xclang) 同一发布版本中的 C++ 工具链与预编译 LLVM/Clang 库，以及 clice 链接的其余库。仓库中的 pixi 和 npm 用于配置测试和文档所需的工具，构建 clice 本身两者都不需要。

## 环境要求

- Bazel，通过 [bazelisk](https://github.com/bazelbuild/bazelisk) 运行，后者会拉取 `.bazelversion` 指定的版本。
- `PATH` 中需有 git 和 [Node.js](https://nodejs.org)：构建会调用 Node 脚本，把 `git describe` 得出的版本号写入 clice。
- Windows 上还需要 Bash（[Git for Windows](https://gitforwindows.org) 自带）以及较短的 Bazel 输出根目录，因为 Bazel 默认的输出根目录层级过深，会超出 Windows 路径长度限制。将其写入 `%USERPROFILE%\.bazelrc`：

  ```
  startup --output_user_root=C:/b
  ```

## 构建

```shell
bazel build //:bin/clice
```

构建出的 clice 位于 `build/RelWithDebInfo/bin/bin/clice`，同级还有资源目录 `build/RelWithDebInfo/bin/lib/clang`，clice 从中读取 clang 的头文件。

各构建类型对应不同的配置，每种配置各有独立目录 `build/<type>`：

| 配置                      | 作用                                                                        |
| ------------------------- | --------------------------------------------------------------------------- |
| `--config=RelWithDebInfo` | 默认配置：开启优化，附带调试信息                                            |
| `--config=Debug`          | 轻度优化（`-O1`）并启用 Address Sanitizer，支持 x86_64 Linux 和 arm64 macOS |

`--platforms=@xclang//platforms:<triple>` 可为其他目标平台构建：在 x86_64 Linux 上可构建其余所有目标平台，例如 `--platforms=@xclang//platforms:aarch64-apple-darwin`。

LLVM/Clang 库中存放的是 ThinLTO bitcode，每次链接程序都会重新执行其代码生成，单个程序耗时数分钟。lld 会将生成结果缓存在 `/var/tmp/xclang-thinlto`（Windows 上为 `C:/xclang-thinlto`），后续链接只需数秒。

`bazel build //:package //:symbols` 会构建发布归档包与符号包（其中包含供 `scripts/symbolize.py` 读取的 `clice.gsym`），产物为 `build/<type>/bin` 下的 `clice.tar.gz` 和 `clice-symbol.tar.xz`（Windows 上为 `.zip`）。

## 开发 clice

`bazel run @compdb//:refresh -- --output_groups=+compilation_prerequisites_INTERNAL_` 会在仓库根目录生成 clice 自身源码的 `compile_commands.json`，以便用 clice 开发其自身代码；该输出组还会构建各编译命令读取的生成头文件。

运行测试需要 pixi 安装的工具。`bazel build //:dist` 会构建 clice 及单元测试，随后 `pixi run test` 会运行单元测试与测试用例（Debug 构建使用 `--config=Debug` 和 `pixi run test Debug`）；测试用例的编写方法见 [`tests/README.md`](https://github.com/clice-io/clice/blob/main/tests/README.md)。

## 使用其他 LLVM 构建

clice 链接其开发时所针对的确切版本的 LLVM/Clang 库，不能使用系统自带的版本。若要换用自行构建的库来替代发布版本，可传入 `--repo_env=XCLANG_LIBCLANG_ROOT=<directory>`（ASan 版本使用 `XCLANG_LIBCLANG_ASAN_ROOT`）；该库必须采用与 xclang 相同的方式构建，即运行 xclang 的 `scripts/toolchain.ts`。
