# 测试与调试

## 运行测试

clice 有四种测试：单元测试、集成测试、冒烟测试和快照测试。

全部测试依赖（集成测试套件和工具所需的 node/npm，以及 scripts/ 所需的 python）均由 pixi 管理，无需另行安装。

### 单元测试

```bash
pixi run unit-test          # default RelWithDebInfo
pixi run unit-test Debug    # debug build
```

等价于：

```bash
./build/RelWithDebInfo/bin/unit_tests --verbose
```

### 集成测试

端到端测试会启动真实的 `clice serve` 实例，并通过 LSP 通信。

```bash
pixi run integration-test          # default RelWithDebInfo
pixi run integration-test Debug    # debug build
```

该套件使用基于 vitest 的 TypeScript（`tests/`），并通过官方的 vscode-languageserver-protocol 栈进行 LSP 通信。等价于：

```bash
cd tests
npm run check   # typecheck (tsc strict) + lint (ESLint)
CLICE_EXECUTABLE=../build/RelWithDebInfo/bin/clice npm test
```

常用变体：

```bash
npx vitest run --config integration/vitest.config.ts integration/server/memory_ownership.test.ts   # one file
```

### 冒烟测试

回放已录制的 LSP 会话，以发现协议处理方面的回归问题。

```bash
pixi run smoke-test          # default RelWithDebInfo
pixi run smoke-test Debug    # debug build
```

等价于：

```bash
node tools/replay.ts tests/smoke/*.jsonl \
    --clice=./build/RelWithDebInfo/bin/clice
```

### 快照测试

功能快照语料库位于 `tests/snap/<feature>/`，源文件与快照并排存放。快照测试套件（`tests/snap/snap.test.ts`，领域逻辑位于 `tools/snap/`）会固定每个 fixture 在其 `verify:` 模式所要求的各条路径上的结果：inspect（每个 fixture 对应一个 `clice inspect` 进程，不涉及服务器）和 server（通过真实服务器回放）。集成测试套件完全不参与快照测试。

```bash
pixi run snap-test          # default RelWithDebInfo
pixi run snap-test Debug    # debug build
```

等价于：

```bash
cd tests
CLICE_EXECUTABLE=../build/RelWithDebInfo/bin/clice npm run snap
```

fixture 可以是单个 `.cpp`，也可以是以 `main.cpp` 为入口的子目录——一个多文件单元，其中同级的源文件（模块接口、头文件和其他源文件）都属于该 fixture。用于说明某项能力的 fixture 位于语料库的章节目录中，命名为 `<section>/NN_name.cpp`（或 `<section>/NN_unit/main.cpp`），并以 `/// # Capability name — details` 文档头开头，后接元数据列表，其中 `status`（`supported`、`partial` 或 `unsupported`）为必填项：章节目录名用作功能页面生成区域的键，两位数字决定条目在该区域中的顺序，文档头内容则用于生成页面（见 `tools/docs/feature.ts`）。用于边界情况且没有文档头的 fixture 放在语料库根目录。整个语料库共用的编译标志写在该语料库的 `corpus.json` 清单中；单个 fixture 可用 `- flags: [...]` 追加自己的编译标志。server 路径每次运行时都会把 fixture 放入一次性工作区（源文件落盘时已剥除 `§` 标注），因此 fixture 之间绝不共享状态；后台索引默认关闭，可由各 fixture 使用 `- indexing: true` 开启，读取的字节与编译器完全相同。有意让编译产生诊断的 fixture 需声明 `- diagnostics: expected`；非预期诊断会使 fixture 失败，声明了该项却未产生任何诊断也同样会失败。

默认情况下，fixture 使用 `verify: both` 和 `snap: shared`：inspect 与 server 两条路径的结果必须逐字节完全一致，并统一记录在一份 `<name>.snap.yml` 中。两条路径确有合理差异的 fixture 会在其 `///` 文档头中声明 `- snap: separate`（并用 `// snap:` 注释说明原因），两条路径的结果分别记录在 `<name>.inspect.snap.yml` 和 `<name>.server.snap.yml` 中。若已知两条路径的分歧是错误的，则声明 `- snap: skip`：该 fixture 不会在任一路径上运行，并且在两条路径达成一致之前不保留任何快照。仅存在于一条路径上的功能（include 和 import 代码补全由 server 响应；索引转储没有对应的 LSP 请求形式）会声明 `- verify: server` 或 `- verify: inspect`，对应路径的结果记录在普通的 `<name>.snap.yml` 中。

`UPDATE_SNAPSHOTS=1` 一次运行即可更新全部内容：inspect 测试先运行，并负责更新共享快照正文；server 侧只能更新自身对应的变体。server 侧的共享快照不匹配表示 server 处理管线与直接功能调用之间确实存在分歧——应当调查原因，而不是通过重新生成快照将其覆盖。

### 运行全部测试

```bash
pixi run test                # runs unit + integration + smoke + snap
pixi run test Debug          # all tests with debug build
```

## 编辑器 E2E 测试

这些冒烟测试使用真实编辑器（无头 Neovim 和 VS Code）测试本地构建的 clice 可执行文件，在两个 fixture（其中包括一个 C++20 模块项目）上覆盖启动、首次诊断、悬停、转到定义和代码补全。CI 在 Linux 上的 `test-editor` 作业中使用编辑器的最新稳定版本运行这些测试，并且有意不锁定版本：该作业用于发现编辑器新版本导致的故障。

```bash
$ pixi run build                  # build/RelWithDebInfo/bin/clice
$ pixi run -e editor editor-test  # nvim + vscode, both fixtures
```

pixi 环境之外的前置条件：

- `nvim`（stable）可在 `PATH` 中找到，供 `nvim-e2e` 使用。
- 系统提供的 `cmake`/`ninja`/`clang`，供 `editor-prepare` 配置基于 CMake 的模块测试夹具（集成测试也基于这一假设）。
- 显示环境（或 `xvfb-run`），以及 Electron 通常依赖的系统库，供 `vscode-e2e` 使用。

## 调试

如果想为 clice 附加调试器，请先以 socket 模式单独启动 clice，然后连接客户端。

```shell
./build/Debug/bin/clice serve --mode socket --port 50051
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
