# 测试与调试

## 运行测试

clice 有两个在每次改动时运行的测试套件：单元测试和用例。兼容性测试需要真实的构建系统和编译器，与编辑器测试一样单独运行。

全部测试依赖（用例和工具所需的 node/npm，以及 scripts/ 所需的 python）均由 pixi 管理，无需另行安装。

### 单元测试

```bash
pixi run unit-test          # default RelWithDebInfo
pixi run unit-test Debug    # debug build
```

等价于：

```bash
./build/RelWithDebInfo/bin/bin/unittests --verbose
```

### 用例

其余测试都是 `tests-next/<topic>/<subtopic>/NN_name/` 下的用例：一个自成一体的小项目、要在它上面运行的检查，以及 `snap.toml`——用例固定下来的全部输出。用例通过 `tools/testing/` 中的框架向真实的 `clice serve`、`clice inspect` 或某个命令发出请求；本文档的功能页面展示的正是带有 `README.md` 的用例。[`tests-next/README.md`](https://github.com/clice-io/clice/blob/main/tests-next/README.md) 是这套测试的约定：用例怎么写、怎么断言、输出怎么读。

```bash
pixi run cases-test          # default RelWithDebInfo
pixi run cases-test Debug    # debug build
```

等价于：

```bash
CLICE_EXECUTABLE=$PWD/build/RelWithDebInfo/bin/bin/clice npm run cases --workspace tests-next
```

`CLICE_CASES=hover/` 只运行某个路径下的用例。`UPDATE_SNAPSHOTS=1` 会重写它们的输出，并列出哪些发生了变化；要像审查代码一样审查这些变化。

改动 TypeScript 时，还要在仓库根目录通过 `npm run check`：对每个包运行严格模式的 tsc 和 ESLint。`npm test --workspace tools` 运行工具自身的测试。

### 运行全部测试

```bash
pixi run test                # runs unit + cases
pixi run test Debug          # all tests with debug build
```

## 编辑器 E2E 测试

这些冒烟测试使用真实编辑器（无头 Neovim 和 VS Code）测试本地构建的 clice 可执行文件，覆盖启动、首次诊断、悬停、转到定义和代码补全，并在头文件上覆盖显示和切换其编译上下文的命令。测试运行在三个用例的副本上——一个单文件项目、一个 C++20 模块项目，以及一个有两个宿主的头文件——这些副本由 `editor-prepare` 写入 `.cache/editors`。CI 在 Linux x64 和 macOS arm64 两条流水线上使用编辑器的最新稳定版本运行这些测试，并且有意不锁定版本：该作业用于发现编辑器新版本导致的故障。

```bash
$ pixi run build                  # build/RelWithDebInfo/bin/bin/clice
$ pixi run -e editor editor-test  # nvim + vscode, all three projects
```

pixi 环境之外的前置条件：

- `nvim`（stable）可在 `PATH` 中找到，供 `nvim-e2e` 使用。
- 显示环境（或 `xvfb-run`），以及 Electron 通常依赖的系统库，供 `vscode-e2e` 使用。

## 兼容性测试

真实的构建系统与真实的编译器：`tests/compat/scenarios.ts` 中的每个场景用一种构建系统和一套工具链构建 `tests/compat/project/` 下的小项目，再让 clice 处理该构建写出的编译数据库。每个翻译单元都必须无错误地解析，正如它在真实编译器下能编译通过；clice 必须与该编译器在命令参数所决定的宏上保持一致，比较由一个生成的头文件在 clice 自己的解析中完成；每个文件的命令都必须经编译器解析成功，并按场景列出的要求保留或去掉相应参数。仓库不提交任何编译数据库：套件检查的是工具当下写出的结果。

```bash
pixi run compat-test          # default RelWithDebInfo
```

每个场景都按 CI 运行器镜像中的样子指定某一平台的编译器：Linux 上是发行版带版本号的 GCC 与 Clang，以及 bear、ccache、meson、ninja、xmake、bazel、zig、Emscripten、MinGW、RISC-V 和 Arm 交叉编译器，还有 pixi `cuda` 环境中的 nvcc（`pixi install -e cuda`）；Windows 上是 Visual Studio、LLVM 和 MinGW；macOS 上是 Apple clang 以及 Homebrew 的 GCC 和 LLVM。缺少工具的场景在本地跳过，在 CI 中则判为失败。clice 尚不支持的场景在 `unsupported` 中写明原因：它的检查被跳过，构建照常运行。CI 每次构建都会运行它，并每周针对最新的发布版本运行一次。

## 调试

如果想为 clice 附加调试器，请先以 socket 模式单独启动 clice，然后连接客户端。

```shell
./build/Debug/bin/bin/clice serve --mode socket --port 50051
```

服务器启动后，可以通过以下两种方式连接客户端：

### 通过 VS Code 连接

配置 clice 扩展，使其连接到正在运行的实例：

1. 安装 [clice](https://marketplace.visualstudio.com/items?itemName=clice-io.clice) 扩展。

2. 配置 `.vscode/settings.json`：

   ```jsonc
   {
     "clice.executable": "/path/to/your/clice/executable",
     "clice.mode": "socket",
     "clice.port": 50051,
     // Optional: disable clangd if also installed
     "clangd.path": "",
   }
   ```

3. 重新加载窗口（`Developer: Reload Window`），使设置生效。

### 调试 VS Code 扩展

扩展位于仓库内的 `editors/vscode/`：

1. 安装依赖：

   ```shell
   npm install # at the repo root; the extension is an npm workspace member
   ```

2. 用 VS Code 打开**仓库根目录**（启动配置位于根目录的 `.vscode/launch.json`）。

3. 创建 `.vscode/settings.json`，使用上述 TCP 配置。

4. 按 `F5` 并选择 `VSCode Extension (pipe)` 或 `VSCode Extension (socket)`，以启动扩展开发宿主窗口。
