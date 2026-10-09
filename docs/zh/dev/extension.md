# 编辑器扩展

本节介绍各编辑器扩展（VS Code / Neovim / Zed）的开发与发布流程。

## VS Code

VS Code 扩展使用 Node/npm/VSCE 工具链。请在 pixi 的 `node` 环境中操作，以确保版本一致。

```shell
# prepare environment (install pixi first)
pixi shell -e node

# install deps (uses package-lock)
pixi run install-vscode

# package the extension; outputs editors/vscode/*.vsix
pixi run build-vscode
```

发布到 VS Code Marketplace（需要 `VSCE_PAT` 环境变量）：

```shell
pixi run publish-vscode
```

> [!IMPORTANT]
> 开发版和本地打包版不会内置 clice 服务端（发布 CI 会按平台将其加入包中），因此请在 VS Code 设置中将 `clice.executable`（或 `CLICE_EXECUTABLE` 环境变量）设为本地构建的二进制文件。否则扩展会报告找不到服务端。

开发与调试：

1. `pixi shell -e node`
2. 在 `editors/vscode` 中运行 `npm run watch`，进行增量构建
3. 在 VS Code 中使用“Run Extension/Launch Extension”配置，或运行 `code --extensionDevelopmentPath=$(pwd)/editors/vscode`

常用脚本（在 `pixi shell -e node` 环境中）：

```bash
npm run package # same as pixi run build-vscode
npm run publish # same as pixi run publish-vscode
```

如果不使用 pixi，请自行安装 node.js >= 22（自带 npm）。该扩展是仓库 npm workspace 的一部分，因此请先在仓库根目录安装依赖，再从 `editors/vscode` 打包：

```bash
npm install          # at the repo root
cd editors/vscode
npm run package
```

## Neovim

Neovim 集成位于 `editors/nvim`：

- `lsp/clice.lua` 是 LSP 配置。nvim-lspconfig 自己的 `lsp/clice.lua` 就是同一份文件，因此这里的改动也要向那边提交 pull request；`lsp/.stylua.toml` 是 nvim-lspconfig 的格式化配置，确保两份文件完全一致。
- `plugin/clice.lua` 淡化显示非活动预处理分支。
- `tests/e2e.lua` 是以 headless 模式运行的冒烟测试：`pixi run -e editor nvim-e2e`。

试用改动时，把该目录追加到 `runtimepath`（`set rtp+=/path/to/clice/editors/nvim`），排在 nvim-lspconfig 之后，从而覆盖它的副本，并调用 `vim.lsp.enable('clice')`；`:checkhealth vim.lsp` 会显示客户端及其日志的位置。

## Zed

Zed 扩展位于 `editors/zed`，使用 Rust 和 `zed_extension_api`。

建议的本地验证流程：

```bash
cd editors/zed
cargo build --release
```

然后按照 Zed 官方指南加载本地扩展（需要 Zed CLI）。启动前请确保 `clice` 位于 `PATH` 中。发布时请遵循 Zed 扩展发布流程。
