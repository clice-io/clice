# Test and Debug

## 运行测试

clice 有三种测试：单元测试、集成测试和冒烟测试。

所有测试依赖（pytest、pygls 等）由 pixi 管理，无需单独安装。

### 单元测试

```bash
pixi run unit-test          # 默认 RelWithDebInfo
pixi run unit-test Debug    # debug 构建
```

等价于：

```bash
./build/RelWithDebInfo/bin/unit_tests \
    --test-dir="./tests/data" \
    --snapshot-dir="./tests/snapshots" \
    --corpus-dir="./tests/corpus" \
    --verbose
```

### 集成测试

启动真实的 clice 服务器，通过 LSP 协议进行端到端测试。

```bash
pixi run integration-test          # 默认 RelWithDebInfo
pixi run integration-test Debug    # debug 构建
```

等价于：

```bash
pytest -s --log-cli-level=INFO --timeout=300 --timeout-method=thread \
    tests/integration --executable=./build/RelWithDebInfo/bin/clice
```

### 冒烟测试

回放录制的 LSP 会话，检查协议处理是否有回归。

```bash
pixi run smoke-test          # 默认 RelWithDebInfo
pixi run smoke-test Debug    # debug 构建
```

等价于：

```bash
python tests/replay.py tests/smoke/*.jsonl \
    --clice=./build/RelWithDebInfo/bin/clice
```

### 运行全部测试

```bash
pixi run test                # 单元 + 集成 + 冒烟
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

如果想在 clice 上附加调试器，推荐先以 tcp 模式单独启动 clice，然后连接客户端。

```shell
./build/Debug/bin/clice serve --mode tcp --port 50051
```

服务器启动后，可以通过以下两种方式连接客户端：

### 通过 VS Code 连接

配置 clice 插件连接到正在运行的实例：

1. 安装 [clice](https://marketplace.visualstudio.com/items?itemName=ykiko.clice-vscode) 插件。

2. 配置 `.vscode/settings.json`：

   ```jsonc
   {
     "clice.executable": "/path/to/your/clice/executable",
     "clice.mode": "tcp",
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

3. 创建上述 tcp 配置的 `.vscode/settings.json`。

4. 按 `F5` 启动扩展开发宿主窗口。
