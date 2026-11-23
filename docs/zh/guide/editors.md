# 编辑器设置

clice 实现了[语言服务器协议（Language Server Protocol）](https://microsoft.github.io/language-server-protocol)，因此任何带有 LSP 客户端的编辑器都可以使用它。下面的编辑器分为两类：有官方 clice 插件的编辑器，以及使用通用 LSP 客户端配置的编辑器。

所有配置的前提：

- `clice` 可执行文件位于 `PATH` 中（或在下面的代码片段中使用绝对路径）；VS Code 扩展自带一份可执行文件，Zed 扩展在找不到时会自动下载一份。
- 项目提供 `compile_commands.json`（clice 默认先搜索项目根目录，再搜索其中每个不含独立 `clice.toml` 或 `.clice/config.toml` 的直接子目录）。

## 编译器环境

clice 会向每条编译命令所用的编译器查询其编译配置（目标平台、系统头文件和默认语言标准），并在编辑器启动 clice 时的环境中运行该编译器。若编译器需要额外的环境变量，可在编辑器中设置 clice 的运行环境：例如 Windows 上的 Cygwin 或 MSYS2 编译器，就需要把其运行时的 `bin` 目录加入 `PATH`。

- VS Code：`clice.serverEnv`，其值用 `${env:NAME}` 引用 VS Code 自身的环境变量，写法与 `terminal.integrated.env.*` 相同：

  ```json
  { "clice.serverEnv": { "PATH": "C:\\cygwin64\\bin;${env:PATH}" } }
  ```

- Neovim：配置 `cmd_env`：

  ```lua
  vim.lsp.config('clice', { cmd_env = { PATH = 'C:\\cygwin64\\bin;' .. vim.env.PATH } })
  ```

- Zed：`lsp.clice.binary.env`，其值按原样使用。

## 官方插件

### Visual Studio Code

从应用市场安装 [clice 扩展](https://marketplace.visualstudio.com/items?itemName=clice-io.clice)；Cursor、VSCodium、Windsurf 等 VS Code 衍生编辑器则从 [Open VSX](https://open-vsx.org/extension/clice-io/clice) 安装同一个扩展。发布的构建版本按平台区分，并在扩展内自带 clice 服务器，因此安装后无需再下载或访问网络；若要改用自行构建的版本，请设置 `clice.executable`。

由 clice 提供服务的编辑器右键菜单中包含 clice 的命令，位于“跳转到定义”等命令下方：**Switch Header and Source (Module Interface and Implementation)**；文件有多个编译上下文时的 **Switch Compilation Context**（资源管理器的 Compilation Contexts 视图中也会列出）；规则声明了构建配置时的 **Switch Build Configuration**，它会以所选配置重启服务器；以及 **Show Compile Command**，它会打开文件完整的编译命令，并说明命令来源——编译数据库、包含该头文件的源文件、规则的 `default_command`、邻近的翻译单元，或 clice 自身的回退配置。耗时较长的编译会在状态栏显示进度，在所有支持进度显示的编辑器中都是如此。

若有其他 C/C++ 语言扩展与 clice 同时运行，代码补全和诊断会重复出现。clice 启动后，会提议在当前工作区中关闭 cpptools（其调试器不受影响）和 clangd 的语言功能；对于没有相应设置的 ccls，则打开扩展视图。警告被忽略后，在该窗口中不会再次出现。若要把某个工作区留给其他扩展，可在该工作区的设置中将 `clice.enable` 设为 `false`：扩展不会在该工作区启动服务器，其命令会提示 clice 已禁用。若在用户设置中设为 `false`，clice 会在所有工作区中关闭，只有重新将其设为 `true` 的工作区除外。修改立即生效，无需重新加载窗口。每个窗口只由一个服务器提供服务，因此多根工作区（multi-root workspace）从其 `.code-workspace` 文件读取该设置，而不是从某个文件夹的设置中读取。

### Neovim

在安装了 [nvim-lspconfig](https://github.com/neovim/nvim-lspconfig) 的 Neovim 0.11 或更高版本上，启用 clice：

```lua
vim.lsp.enable('clice')
```

nvim-lspconfig 收录了 [`editors/nvim`](https://github.com/clice-io/clice/tree/main/editors/nvim) 中配置的一份副本，更新晚于仓库本身；如果未安装 nvim-lspconfig，或者想先用上其副本尚未同步的改动，可将该目录下的 `lsp/clice.lua` 复制到你配置目录的 `after/lsp/` 下，它的优先级高于 nvim-lspconfig 自带的那份。该配置提供了查看和切换缓冲区编译上下文的命令；[README](https://github.com/clice-io/clice/tree/main/editors/nvim#readme) 列出了这些命令，以及 Neovim 留给用户自行配置的功能，例如内联提示和输入时自动补全。

### Zed

Zed 扩展位于 [`editors/zed`](https://github.com/clice-io/clice/tree/main/editors/zed)。若设置了 `lsp.clice.binary.path` 则运行它，参数取自 `lsp.clice.binary.arguments`，其中必须包含 `serve`；否则运行 `clice`（从 `PATH` 查找），若也没有则下载最新发布版本；将 `lsp.clice.settings.release_channel` 设为 `"pre-release"` 则改为跟随每日构建。

Zed 还会为 C 和 C++ 运行内置的 clangd，导致代码补全和诊断重复出现；若只使用 clice：

```json
{
  "languages": {
    "C": { "language_servers": ["clice", "!clangd", "..."] },
    "C++": { "language_servers": ["clice", "!clangd", "..."] }
  }
}
```

Zed 默认用 tree-sitter 生成大纲面板和面包屑导航；若要让它们显示 clice 的文档符号，请开启 C 和 C++ 的 `document_symbols`（在 Zed 的 `settings.json` 中设置）：

```json
{
  "languages": {
    "C": { "document_symbols": "on" },
    "C++": { "document_symbols": "on" }
  }
}
```

## 通用 LSP 客户端

没有专属 clice 插件的编辑器（包括 Zed）可在代码操作菜单中切换文件的编译方式：请求代码操作时，clice 会提供源代码操作，用于让头文件改按包含它的另一个宿主文件编译，或让源文件改用它的另一条编译命令（`source.clice.context`）、恢复为 clice 自动选择的上下文，以及为下次启动选择另一个构建配置（`source.clice.configuration`）。所有编辑器都会显示建立索引和长耗时编译的进度，以及提示用户修正配置的消息。

### Helix

在 `~/.config/helix/languages.toml` 中添加：

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

打开 `Settings → Configure Kate → LSP Client → User Server Settings` 并添加：

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
