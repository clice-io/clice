# Editor Setup

clice implements the [Language Server Protocol](https://microsoft.github.io/language-server-protocol), so any editor with an LSP client can use it. The editors below fall into two groups: editors with an official clice plugin, and editors configured through their generic LSP client.

All setups assume:

- The `clice` executable is on your `PATH` (or use an absolute path in the snippets below).
- Your project provides a `compile_commands.json` (by default clice searches the workspace root, then each of its immediate subdirectories).

## Official Plugins

### Visual Studio Code

Install the [clice extension](https://marketplace.visualstudio.com/items?itemName=clice-io.clice) from the marketplace. Marketplace builds are platform-specific and ship the clice server inside the extension, so no download or network access is needed after installation; set `clice.executable` to use your own build instead.

### Neovim

clice ships an LSP config for Neovim ≥ 0.11 in [`editors/nvim`](https://github.com/clice-io/clice/tree/main/editors/nvim). Copy `doc/clice.lua` into your config's `lsp/` directory, then enable it:

```lua
vim.lsp.enable('clice')
```

### Zed

The Zed extension lives in [`editors/zed`](https://github.com/clice-io/clice/tree/main/editors/zed).

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
