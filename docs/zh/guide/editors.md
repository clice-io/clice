# 编辑器设置

clice 实现了 [Language Server Protocol](https://microsoft.github.io/language-server-protocol)，所以任何带 LSP 客户端的编辑器都可以使用它。下面的编辑器分为两类：有官方 clice 插件的编辑器，以及通过通用 LSP 客户端配置的编辑器。

所有配置的前提：

- `clice` 可执行文件位于 `PATH` 中（或在下面的代码片段中使用绝对路径）。
- 你的项目提供 `compile_commands.json`（clice 默认先搜索工作区根目录，再搜索其每个直接子目录）。

## 官方插件

### Visual Studio Code

从应用市场安装 [clice 扩展](https://marketplace.visualstudio.com/items?itemName=clice-io.clice)。应用市场中的构建版本针对不同平台提供，并在扩展中内置 clice 服务端，因此安装后无需再下载或访问网络；若要改用自己的构建版本，请设置 `clice.executable`。

### Neovim

clice 在 [`editors/nvim`](https://github.com/clice-io/clice/tree/main/editors/nvim) 中随附了适用于 Neovim ≥ 0.11 的 LSP 配置。将 `doc/clice.lua` 复制到配置目录中的 `lsp/` 目录，然后启用：

```lua
vim.lsp.enable('clice')
```

### Zed

Zed 扩展位于 [`editors/zed`](https://github.com/clice-io/clice/tree/main/editors/zed)。

## 通用 LSP 客户端

### Helix

在 `~/.config/helix/languages.toml` 中添加以下内容：

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

安装 [LSP 包](https://packagecontrol.io/packages/LSP)，然后在其设置中添加以下内容：

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

打开 `Settings → Configure Kate → LSP Client → User Server Settings`，然后添加以下内容：

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
