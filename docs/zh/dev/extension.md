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

如果不使用 pixi，请自行安装 node.js >= 20（自带 npm）。该扩展是仓库 npm workspace 的一部分，因此请先在仓库根目录安装依赖，再从 `editors/vscode` 打包：

```bash
npm install          # at the repo root
cd editors/vscode
npm run package
```

## Neovim

Neovim 扩展位于 `editors/nvim`，使用 Lua 编写，目前仍在持续开发。

- 将仓库路径加入 `runtimepath`，例如：`set rtp+=/path/to/clice/editors/nvim`
- 或创建本地符号链接：`~/.config/nvim/pack/clice/start/clice` -> `<repo>/editors/nvim`
- 确保能通过 `$PATH` 找到 `clice` 可执行文件

开发提示：代码库规模较小，可直接在 Neovim 中加载，并查看 `:messages`/LSP 日志；使用 `stylua` 进行格式化（已包含配置）。

## Zed

Zed 扩展位于 `editors/zed`，使用 Rust 和 `zed_extension_api`。

建议的本地验证流程：

```bash
cd editors/zed
cargo build --release
```

然后按照 Zed 官方指南加载本地扩展（需要 Zed CLI）。启动前请确保 `clice` 位于 `PATH` 中。发布时请遵循 Zed 扩展发布流程。
