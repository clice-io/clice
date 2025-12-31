# Extension

This section covers development and release workflows for the editor extensions (VSCode / Neovim / Zed).

## VSCode

The VSCode extension uses the Node/PNPM/VSCE toolchain. Work inside the pixi `node` environment for consistent versions.

```shell
# prepare environment (install pixi first)
pixi shell -e node

# install deps (uses pnpm-lock)
pixi run install-vscode

# package the extension; outputs editors/vscode/*.vsix
pixi run build-vscode
```

Publish to the VSCode Marketplace (`VSCE_PAT` env var required):

```shell
pixi run publish-vscode
```

> [!TIP]
> If clice is already built locally, set `clice.executable` in VSCode settings to point the extension to your custom binary.

Develop and debug:

1. `pixi shell -e node`
2. In `editors/vscode`, run `pnpm run watch` for incremental builds
3. In VSCode, use the “Run Extension/Launch Extension” configs, or run `code --extensionDevelopmentPath=$(pwd)/editors/vscode`

Common scripts (inside `pixi shell -e node`):

```bash
pnpm run package # same as pixi run build-vscode
pnpm run publish # same as pixi run publish-vscode
```

If you skip pixi, install node.js >= 20 and pnpm yourself, then in `editors/vscode` run:

```bash
pnpm install
pnpm run package
```

## Neovim

The Neovim extension lives in `editors/nvim` and is written in Lua. It is still evolving.

- Add the repo path to `runtimepath`, e.g. `set rtp+=/path/to/clice/editors/nvim`
- Or create a local symlink: `~/.config/nvim/pack/clice/start/clice` -> `<repo>/editors/nvim`
- Ensure the `clice` executable is discoverable in `$PATH`

Dev tips: the codebase is small—load it directly in Neovim and watch `:messages`/LSP logs; format with `stylua` (config included).

## Zed

The Zed extension lives in `editors/zed` and uses Rust plus `zed_extension_api`.

Suggested local verification:

```bash
cd editors/zed
cargo build --release
```

Then load the local extension per Zed's official guide (Zed CLI required). Make sure `clice` is on `PATH` before launching. Follow the Zed extension publishing flow when releasing.
