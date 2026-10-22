# 编辑器设置

clice 实现了 [Language Server Protocol](https://microsoft.github.io/language-server-protocol)，所以任何带 LSP 客户端的编辑器都可以使用它。下面的编辑器分为两类：有官方 clice 插件的编辑器，以及通过通用 LSP 客户端配置的编辑器。

所有配置的前提：

- `clice` 可执行文件位于 `PATH` 中（或在下面的代码片段中使用绝对路径）。
- 你的项目提供 `compile_commands.json`（clice 默认先搜索工作区根目录，再搜索其每个直接子目录）。

## 编译器环境

clice 会询问每条命令的编译器如何编译（它的目标、系统头文件和默认标准），并在编辑器启动 clice 时所用的环境中运行编译器。需要更多环境的编译器，可以通过编辑器中设置 clice 环境的选项获得：例如，Windows 上的 Cygwin 或 MSYS2 编译器需要其运行时的 `bin` 目录位于 `PATH` 中。

- VS Code：`clice.serverEnv`，其值用 `${env:NAME}` 引用 VS Code 自身的环境变量，与 `terminal.integrated.env.*` 的写法相同：

  ```json
  { "clice.serverEnv": { "PATH": "C:\\cygwin64\\bin;${env:PATH}" } }
  ```

- Neovim：`cmd_env`：

  ```lua
  vim.lsp.config('clice', { cmd_env = { PATH = 'C:\\cygwin64\\bin;' .. vim.env.PATH } })
  ```

- Zed：`lsp.clice.binary.env`，其值按原样使用。

## 官方插件

### Visual Studio Code

从应用市场安装 [clice 扩展](https://marketplace.visualstudio.com/items?itemName=clice-io.clice)；Cursor、VSCodium、Windsurf 等 VS Code 衍生编辑器从 [Open VSX](https://open-vsx.org/extension/clice-io/clice) 安装同一个扩展。发布的构建版本针对不同平台提供，并在扩展中内置 clice 服务端，因此安装后无需再下载或访问网络；若要改用自己的构建版本，请设置 `clice.executable`。

### Neovim

在 Neovim 0.11 或更高版本上安装 [nvim-lspconfig](https://github.com/neovim/nvim-lspconfig) 后，启用 clice：

```lua
vim.lsp.enable('clice')
```

nvim-lspconfig 收录了 [`editors/nvim`](https://github.com/clice-io/clice/tree/main/editors/nvim) 中配置的一份副本，更新晚于这里；未安装 nvim-lspconfig，或想先用上它副本里还没有的改动时，把其中的 `lsp/clice.lua` 复制到你配置目录中的 `after/lsp/` 目录，它优先于 nvim-lspconfig 的那份。该配置添加了几个命令，用于查看和切换缓冲区的编译上下文；[README](https://github.com/clice-io/clice/tree/main/editors/nvim#readme) 列出了这些命令，以及 Neovim 留给你自行配置的功能，例如内联提示和输入时自动补全。

### Zed

Zed 扩展位于 [`editors/zed`](https://github.com/clice-io/clice/tree/main/editors/zed)。

Zed 默认用 tree-sitter 生成大纲面板和面包屑；要让它们显示 clice 给出的文档符号，在 Zed 的 `settings.json` 里为 C 和 C++ 打开 `document_symbols`：

```json
{
  "languages": {
    "C": { "document_symbols": "on" },
    "C++": { "document_symbols": "on" }
  }
}
```

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
