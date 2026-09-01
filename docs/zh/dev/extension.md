# Extension

本节汇总各编辑器扩展（VSCode / Neovim / Zed）的开发与发布流程。

## VSCode

VSCode 扩展使用 Node/npm/VSCE 工具链。请在 pixi 的 `node` 环境中操作，以确保版本一致。

```shell
# prepare environment (install pixi first)
pixi shell -e node

# install deps (uses package-lock)
pixi run install-vscode

# package the extension; outputs editors/vscode/*.vsix
pixi run build-vscode
```

发布到 VSCode Marketplace（需要 `VSCE_PAT` 环境变量）：

```shell
pixi run publish-vscode
```

> [!IMPORTANT]
> 开发构建和本地打包的扩展不会内置 clice 服务端（发布 CI 才会按平台注入），所以请在 VSCode 设置中配置 `clice.executable`（或 `CLICE_EXECUTABLE` 环境变量）指向本地构建的二进制。否则扩展会提示找不到服务端。

开发与调试：

1. `pixi shell -e node`
2. 在 `editors/vscode` 下运行 `npm run watch`（增量构建）
3. 在 VSCode 中使用“Run Extension/Launch Extension”配置，或运行 `code --extensionDevelopmentPath=$(pwd)/editors/vscode`

常用脚本（在 `pixi shell -e node` 下）：

```bash
npm run package # same as pixi run build-vscode
npm run publish # same as pixi run publish-vscode
```

如果不使用 pixi，请自行安装 node.js >= 20（自带 npm）。扩展是仓库 npm workspace 的一部分，请在仓库根目录安装依赖，然后在 `editors/vscode` 下打包：

```bash
npm install          # at the repo root
cd editors/vscode
npm run package
```

## Neovim

Neovim 扩展位于 `editors/nvim`，使用 Lua 编写。目前仍在演进中。

- 将仓库路径加入 `runtimepath`，例如：`set rtp+=/path/to/clice/editors/nvim`
- 或创建本地符号链接：`~/.config/nvim/pack/clice/start/clice` -> `<repo>/editors/nvim`
- 确保 `clice` 可执行文件能在 `$PATH` 中找到

开发提示：代码量较小——直接在 Neovim 中加载，观察 `:messages`/LSP 日志；使用 `stylua` 格式化（配置已包含）。

## Zed

Zed 扩展位于 `editors/zed`，使用 Rust 和 `zed_extension_api`。

建议的本地验证流程：

```bash
cd editors/zed
cargo build --release
```

然后按 Zed 官方指南加载本地扩展（需要 Zed CLI）。启动前确保 `clice` 在 `PATH` 中。发布时遵循 Zed 扩展发布流程。
