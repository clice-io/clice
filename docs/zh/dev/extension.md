# Extension

本节汇总各编辑器插件的开发与发布流程。目前包含 VSCode / Neovim / Zed。

## VSCode

VSCode 插件使用 Node/PNPM/VSCE 链路。推荐在 pixi 的 `node` 环境下操作以获得一致的工具链版本。

```shell
# 准备环境（先安装 pixi）
pixi shell -e node

# 安装依赖（基于 pnpm-lock）
pixi run install-vscode

# 打包扩展，产物位于 editors/vscode/*.vsix
pixi run build-vscode
```

发布到 VSCode Marketplace（需要 `VSCE_PAT` 环境变量）：

```shell
pixi run publish-vscode
```

> [!TIP]
> 若已编译 clice，本地调试时可在 VSCode 设置中填写 `clice.executable`，使扩展指向你的自定义构建。

开发与调试：

1. `pixi shell -e node`
2. 在 `editors/vscode` 下运行 `pnpm run watch`（增量构建）
3. VSCode 中使用 “Run Extension/Launch Extension” 调试配置，或执行 `code --extensionDevelopmentPath=$(pwd)/editors/vscode`

常用脚本（在 `pixi shell -e node` 下）：

```bash
pnpm run package # 等价于 pixi run build-vscode
pnpm run publish # 等价于 pixi run publish-vscode
```

如果不使用 pixi，请自行准备 node.js >= 20、pnpm，然后在 `editors/vscode` 目录执行：

```bash
pnpm install
pnpm run package
```

## Neovim

Neovim 插件位于 `editors/nvim`，使用 Lua 编写。目前功能仍在演进中。

- 将仓库路径加入 `runtimepath`，例如：`set rtp+=/path/to/clice/editors/nvim`
- 或在本地创建软链接：`~/.config/nvim/pack/clice/start/clice` -> `<repo>/editors/nvim`
- 需要 `clice` 可执行文件可在 `$PATH` 中被找到

开发提示：代码量较小，可直接在 Neovim 中加载并通过 `:messages`/LSP 日志观察效果；格式化可使用 `stylua`（仓库中已提供 `stylua.toml`）。

## Zed

Zed 插件位于 `editors/zed`，使用 Rust 和 `zed_extension_api`。

建议的本地验证流程：

```bash
cd editors/zed
cargo build --release
```

随后按 Zed 官方指南加载本地扩展（需安装 Zed CLI），在启动前确保 `clice` 已在 PATH 中。发布时同样遵循 Zed 扩展发布流程。
