# Editor Setup

clice 实现了 [Language Server Protocol](https://microsoft.github.io/language-server-protocol)，任何带 LSP 客户端的编辑器都可以使用它。下面的编辑器分为两类：有官方 clice 插件的，以及通过通用 LSP 客户端配置的。

所有配置的前提：

- `clice` 可执行文件在 `PATH` 中（或在下面的片段中使用绝对路径）。
- 项目提供 `compile_commands.json`（clice 默认先搜索工作区根目录，再依次搜索其各个直接子目录）。

## Official Plugins

### Visual Studio Code

从市场安装 [clice 扩展](https://marketplace.visualstudio.com/items?itemName=ykiko.clice-vscode)。扩展会自动下载 clice 二进制；如需使用自己构建的版本，设置 `clice.executable`。

### Neovim

clice 在 [`editors/nvim`](https://github.com/clice-io/clice/tree/main/editors/nvim) 中提供了 Neovim ≥ 0.11 的 LSP 配置。把 `doc/clice.lua` 复制到你配置目录的 `lsp/` 下，然后启用：

```lua
vim.lsp.enable('clice')
```

### Zed

Zed 扩展位于 [`editors/zed`](https://github.com/clice-io/clice/tree/main/editors/zed)。

## Generic LSP Clients

### Helix

添加到 `~/.config/helix/languages.toml`：

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

使用内置的 eglot：

```elisp
(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               '((c-mode c-ts-mode c++-mode c++-ts-mode)
                 . ("clice" "serve"))))
```

### Sublime Text

安装 [LSP 包](https://packagecontrol.io/packages/LSP)，然后在其设置中添加：

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

打开 `设置 → 配置 Kate → LSP 客户端 → 用户服务器设置`，添加：

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

使用 [vim-lsp](https://github.com/prabirshrestha/vim-lsp)：

```vim
if executable('clice')
    au User lsp_setup call lsp#register_server({
        \ 'name': 'clice',
        \ 'cmd': {server_info->['clice', 'serve']},
        \ 'allowlist': ['c', 'cpp'],
        \ })
endif
```
