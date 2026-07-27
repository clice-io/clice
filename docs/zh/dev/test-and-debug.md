# Test and Debug

## 运行测试

clice 有四种测试：单元测试、集成测试、冒烟测试和快照测试（snap tests）。

所有测试依赖（集成套件与工具的 node/npm、scripts/ 的 python）由 pixi 管理，无需单独安装。

### 单元测试

```bash
pixi run unit-test          # 默认 RelWithDebInfo
pixi run unit-test Debug    # debug 构建
```

等价于：

```bash
./build/RelWithDebInfo/bin/unit_tests \
    --test-dir="./tests/data" \
    --snapshot-dir="./tests/snapshots/unit" \
    --verbose
```

### 集成测试

启动真实的 clice 服务器，通过 LSP 协议进行端到端测试。

```bash
pixi run integration-test          # 默认 RelWithDebInfo
pixi run integration-test Debug    # debug 构建
```

集成套件是基于 vitest 的 TypeScript（`tests/`），通过官方
vscode-languageserver-protocol 栈与 server 通信。等价于：

```bash
cd tests
npm run check   # 类型检查（tsc strict）+ lint（ESLint）
CLICE_EXECUTABLE=../build/RelWithDebInfo/bin/clice npm test
```

常用变体：

```bash
npx vitest run --config integration/vitest.config.ts integration/server/memory_ownership.test.ts   # 单文件
```

### 冒烟测试

回放录制的 LSP 会话，检查协议处理是否有回归。

```bash
pixi run smoke-test          # 默认 RelWithDebInfo
pixi run smoke-test Debug    # debug 构建
```

等价于：

```bash
node tools/replay.ts tests/smoke/*.jsonl \
    --clice=./build/RelWithDebInfo/bin/clice
```

### 快照测试

Feature 快照语料位于 `tests/snap/<feature>/`，源文件与快照并排存放。snap 套件（`tests/snap.test.ts`，领域逻辑在 `tools/snap/`）从两条路径固定每个 fixture：standalone（每个 fixture 一个 `clice inspect` 进程，不经过 server）与 wire（通过真实 server 回放）。集成套件不参与任何快照。

```bash
pixi run snap-test          # 默认 RelWithDebInfo
pixi run snap-test Debug    # debug 构建
```

等价于：

```bash
cd tests
CLICE_EXECUTABLE=../build/RelWithDebInfo/bin/clice npm run snap
```

fixture 默认是 `snap: shared`：standalone 与 wire 两路结果必须渲染成逐字节一致的同一份 `<name>.snap.yml`。两路确有合理差异的 fixture 在 `///` 文档头部声明 `- snap: separate`（并用 `// snap:` 注释说明原因），wire 结果单独固定在 `<name>.wire.snap.yml`。两路分歧属于明确错误（尚不支持）的，声明 `- snap: skip`：两个套件都跳过该 fixture，且在两路一致之前不保留任何快照。

`UPDATE_SNAPSHOTS=1` 一次跑完即可完成全部更新：standalone 测试先执行并拥有共享快照内容；wire 一侧只能更新 `.wire.snap.yml` 变体。wire 侧报告共享快照不匹配 = server 管线与直接调用 feature 之间的真实分歧——应当排查，而不是重新生成把它盖掉。

### 运行全部测试

```bash
pixi run test                # 单元 + 集成 + 冒烟 + 快照
pixi run test Debug          # debug 构建的全部测试
```

## Editor E2E Tests

冒烟测试，用真实编辑器（无头 Neovim 和 VSCode）对本地构建的 clice 二进制进行测试，在两个 fixture（包括一个 C++20 模块项目）上覆盖启动、首批诊断、hover、definition 和 completion。CI 在 Linux 的 `test-editor` job 中使用最新 stable 版编辑器运行，刻意不固定版本：这个 job 的目的就是发现新版编辑器导致的破坏。

```bash
$ pixi run build                  # build/RelWithDebInfo/bin/clice
$ pixi run -e editor editor-test  # nvim + vscode，两个 fixture
```

pixi 环境之外的依赖：

- `nvim`（stable）在 `PATH` 中，供 `nvim-e2e` 使用。
- 系统的 `cmake`/`ninja`/`clang`，供 `editor-prepare` 配置基于 CMake 的模块 fixture（与集成测试相同的假设）。
- 显示环境（或 `xvfb-run`）以及 Electron 所需的系统库，供 `vscode-e2e` 使用。

## 调试

如果想在 clice 上附加调试器，推荐先以 socket 模式单独启动 clice，然后连接客户端。

```shell
./build/Debug/bin/clice serve --mode socket --port 50051
```

服务器启动后，可以通过以下两种方式连接客户端：

### 通过 VS Code 连接

配置 clice 插件连接到正在运行的实例：

1. 安装 [clice](https://marketplace.visualstudio.com/items?itemName=clice-io.clice) 插件。

2. 配置 `.vscode/settings.json`：

   ```jsonc
   {
     "clice.executable": "/path/to/your/clice/executable",
     "clice.mode": "socket",
     "clice.port": 50051,
     // 可选：禁用 clangd
     "clangd.path": "",
   }
   ```

3. 重新加载窗口（`Developer: Reload Window`）使设置生效。

### 调试 VS Code 插件

插件位于仓库内 `editors/vscode/`：

1. 安装依赖：

   ```shell
   cd editors/vscode
   pnpm install
   ```

2. 用 VS Code 打开 `editors/vscode`。

3. 创建上述 socket 配置的 `.vscode/settings.json`。

4. 按 `F5` 启动扩展开发宿主窗口。
