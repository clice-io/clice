# Editor Setup

clice implements the [Language Server Protocol](https://microsoft.github.io/language-server-protocol), so any editor with an LSP client can use it. The editors below fall into two groups: editors with an official clice plugin, and editors configured through their generic LSP client.

All setups assume:

- The `clice` executable is on your `PATH` (or use an absolute path in the snippets below).
- Your project provides a `compile_commands.json` (by default clice searches the workspace root, then each of its immediate subdirectories).

## Compiler Environment

clice asks the compiler of each command how it compiles (its target, its system headers, its default standard), and runs it in the environment the editor starts clice with. A compiler that needs more gets it from the editor's setting for clice's environment: a Cygwin or MSYS2 compiler on Windows, for one, needs its runtime's `bin` directory on `PATH`.

- VS Code: `clice.serverEnv`, whose values name VS Code's own variables as `${env:NAME}`, as `terminal.integrated.env.*` does:

  ```json
  { "clice.serverEnv": { "PATH": "C:\\cygwin64\\bin;${env:PATH}" } }
  ```

- Neovim: `cmd_env`:

  ```lua
  vim.lsp.config('clice', { cmd_env = { PATH = 'C:\\cygwin64\\bin;' .. vim.env.PATH } })
  ```

- Zed: `lsp.clice.binary.env`, whose values are taken as written.

## Official Plugins

### Visual Studio Code

Install the [clice extension](https://marketplace.visualstudio.com/items?itemName=clice-io.clice) from the marketplace; Cursor, VSCodium, Windsurf and other VS Code forks install the same extension from [Open VSX](https://open-vsx.org/extension/clice-io/clice). Published builds are platform-specific and ship the clice server inside the extension, so no download or network access is needed after installation; set `clice.executable` to use your own build instead.

The language status of a C or C++ file (the `{}` beside its language mode) tells what clice knows of it; pin an item to keep it in the status bar. It names where the file's compile command comes from — a compilation database, the source including a header, a rule's `default_command`, a unit nearby, or clice's own — and **Show** opens the whole command; it names the compilation context when the file has more than one, and the build configuration when the rules declare any, each with **Switch**. A compile that runs long shows its progress in the status bar, as it does in every editor that shows progress.

### Neovim

On Neovim 0.11 or later with [nvim-lspconfig](https://github.com/neovim/nvim-lspconfig) installed, enable clice:

```lua
vim.lsp.enable('clice')
```

nvim-lspconfig carries a copy of the config in [`editors/nvim`](https://github.com/clice-io/clice/tree/main/editors/nvim), updated after it; without nvim-lspconfig or ahead of its copy, copy `lsp/clice.lua` from there into the `after/lsp/` directory of your config, which takes precedence over nvim-lspconfig's. The config adds commands that show and switch the compilation context of a buffer; the [README](https://github.com/clice-io/clice/tree/main/editors/nvim#readme) lists them, along with the features Neovim leaves to your config, such as inlay hints and completion as you type.

### Zed

The Zed extension lives in [`editors/zed`](https://github.com/clice-io/clice/tree/main/editors/zed). It runs `clice` from `PATH`, else downloads the newest release; `lsp.clice.settings.release_channel` set to `"pre-release"` follows the nightly builds instead.

Zed runs its built-in clangd for C and C++ as well, which duplicates completion and diagnostics; to use clice alone:

```json
{
  "languages": {
    "C": { "language_servers": ["clice", "!clangd", "..."] },
    "C++": { "language_servers": ["clice", "!clangd", "..."] }
  }
}
```

Zed builds its outline panel and breadcrumbs with tree-sitter by default; to have them show clice's document symbols, turn on `document_symbols` for C and C++ in Zed's `settings.json`:

```json
{
  "languages": {
    "C": { "document_symbols": "on" },
    "C++": { "document_symbols": "on" }
  }
}
```

## Generic LSP Clients

### Helix

Add to `~/.config/helix/languages.toml`:

```toml
[language-server.clice]
command = "clice"
args = ["serve"]

[[language]]
name = "cpp"
language-servers = ["clice"]

[[language]]
name = "c"
language-servers = ["clice"]
```

### Emacs

With the built-in eglot:

```elisp
(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               '((c-mode c-ts-mode c++-mode c++-ts-mode)
                 . ("clice" "serve"))))
```

### Sublime Text

Install the [LSP package](https://packagecontrol.io/packages/LSP), then add to its settings:

```json
{
  "clients": {
    "clice": {
      "enabled": true,
      "command": ["clice", "serve"],
      "selector": "source.c | source.c++"
    }
  }
}
```

### Kate

Open `Settings → Configure Kate → LSP Client → User Server Settings` and add:

```json
{
  "servers": {
    "c": {
      "command": ["clice", "serve"],
      "url": "https://github.com/clice-io/clice",
      "highlightingModeRegex": "^(C|C\\+\\+)$"
    }
  }
}
```

### Vim

With [vim-lsp](https://github.com/prabirshrestha/vim-lsp):

```vim
if executable('clice')
    au User lsp_setup call lsp#register_server({
        \ 'name': 'clice',
        \ 'cmd': {server_info->['clice', 'serve']},
        \ 'allowlist': ['c', 'cpp'],
        \ })
endif
```
