# Extension

This section covers development and release workflows for the editor extensions (VSCode / Neovim / Zed).

## VSCode

The VSCode extension uses the Node/npm/VSCE toolchain. Work inside the pixi `node` environment for consistent versions.

```shell
# prepare environment (install pixi first)
pixi shell -e node

# install deps (uses package-lock)
pixi run install-vscode

# package the extension; outputs editors/vscode/*.vsix
pixi run build-vscode
```

Publish to the VSCode Marketplace (`VSCE_PAT` env var required):

```shell
pixi run publish-vscode
```

> [!IMPORTANT]
> Development and locally packaged builds do not bundle the clice server (release CI stages it per platform), so set `clice.executable` in VSCode settings — or the `CLICE_EXECUTABLE` environment variable — to your locally built binary. Without it the extension reports a missing server.

Develop and debug:

1. `pixi shell -e node`
2. In `editors/vscode`, run `npm run watch` for incremental builds
3. In VSCode, use the “Run Extension/Launch Extension” configs, or run `code --extensionDevelopmentPath=$(pwd)/editors/vscode`

Common scripts (inside `pixi shell -e node`):

```bash
npm run package # same as pixi run build-vscode
npm run publish # same as pixi run publish-vscode
```

If you skip pixi, install node.js >= 22 yourself (npm is bundled). The extension is part of the repo's npm workspace, so install at the repo root, then package from `editors/vscode`:

```bash
npm install          # at the repo root
cd editors/vscode
npm run package
```

## Neovim

The Neovim integration lives in `editors/nvim`:

- `lsp/clice.lua` is the LSP config. nvim-lspconfig carries the same file as its own `lsp/clice.lua`, so a change here is also a pull request there; `lsp/.stylua.toml` is nvim-lspconfig's formatting, which keeps the two files identical.
- `plugin/clice.lua` dims inactive preprocessor branches.
- `tests/e2e.lua` is the headless smoke test: `pixi run -e editor nvim-e2e`.

To try a change, append the directory to `runtimepath` (`set rtp+=/path/to/clice/editors/nvim`), after nvim-lspconfig whose copy it then overrides, and `vim.lsp.enable('clice')`; `:checkhealth vim.lsp` shows the client and where its log is.

## Zed

The Zed extension lives in `editors/zed` and uses Rust plus `zed_extension_api`.

Suggested local verification:

```bash
cd editors/zed
cargo build --release
```

Then load the local extension per Zed's official guide (Zed CLI required). Make sure `clice` is on `PATH` before launching. Follow the Zed extension publishing flow when releasing.
