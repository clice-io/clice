# clice for Neovim

Neovim 0.11 or later.

## Setup

[nvim-lspconfig](https://github.com/neovim/nvim-lspconfig) carries a copy of [`lsp/clice.lua`](lsp/clice.lua), updated after this one; with it installed:

```lua
vim.lsp.enable('clice')
```

For this directory's config, without nvim-lspconfig or ahead of its copy, copy `lsp/clice.lua` into the `after/lsp/` directory of your config, which takes precedence over nvim-lspconfig's; or append this directory to `runtimepath`, which also dims inactive preprocessor branches (`plugin/clice.lua`). The config runs `clice` from `PATH`; to run another binary:

```lua
vim.lsp.config('clice', { cmd = { '/path/to/clice', 'serve' } })
```

Neovim runs a clice for each project root: the nearest directory above the file that holds a `clice.toml`, else a `compile_commands.json`, else a `.git` (before Neovim 0.11.3, the nearest that holds any of them).

## Commands

| Command                        | Does                                                                                                                                                   |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `:LspCliceShowContext`         | Show the compilation context of the buffer: the source file a header compiles in, or the compile command of a source file listed with several.         |
| `:LspCliceSwitchContext`       | Pick another context, or go back to the automatic one.                                                                                                 |
| `:LspCliceResetContext`        | Go back to the automatic context.                                                                                                                      |
| `:LspCliceSwitchConfiguration` | Select the build configuration (the `configuration` tags of the `clice.toml` rules) clice runs from its next start.                                    |
| `:LspCliceRefactor {kind}`     | Apply the refactoring of a code action kind at the cursor, such as `refactor.rewrite.populateSwitch`, offering a choice when several apply.            |
| `:LspCliceSwitchSourceHeader`  | Open the file the buffer pairs with — a header's source, a source's header, a module's interface or implementation — or pick one when several qualify. |

## Features

Neovim drives most of clice through its defaults: pulled diagnostics with clang-tidy findings and their fixes (`gra`), semantic tokens, `K` hover, `<C-S>` signature help, `<C-X><C-O>` completion, `<C-]>` definition (on an `#include` too), `grr` references, `gri` implementations, `grn` rename, `gO` document symbols and `gq` formatting. Neovim 0.12 adds `grt` type definition (on `auto`, the deduced type), and `an` and `in` grow and shrink the visual selection through clice in buffers without a tree-sitter parser (Neovim bundles one for C, not for C++).

Inactive preprocessor branches carry the `@lsp.mod.inactive` highlight; `plugin/clice.lua` links it to `Comment`. Without the plugin:

```lua
vim.api.nvim_set_hl(0, '@lsp.mod.inactive', { link = 'Comment', default = true })
```

The rest is opt-in:

```lua
vim.api.nvim_create_autocmd('LspAttach', {
  callback = function(args)
    local client = assert(vim.lsp.get_client_by_id(args.data.client_id))
    if client.name ~= 'clice' then
      return
    end
    vim.lsp.inlay_hint.enable(true, { bufnr = args.buf })
    -- `import ` completes module names.
    vim.lsp.completion.enable(true, client.id, args.buf, { autotrigger = true })
    local group = vim.api.nvim_create_augroup('clice-highlight-' .. args.buf, {})
    vim.api.nvim_create_autocmd('CursorHold', { group = group, buffer = args.buf, callback = vim.lsp.buf.document_highlight })
    vim.api.nvim_create_autocmd('CursorMoved', { group = group, buffer = args.buf, callback = vim.lsp.buf.clear_references })
  end,
})
```

Folding by clice's ranges, with their collapsed text, is `vim.lsp.foldexpr()` and `vim.lsp.foldtext()` (`:help vim.lsp.foldexpr()`). Call and type hierarchies are `vim.lsp.buf.incoming_calls()`, `outgoing_calls()` and `typehierarchy('subtypes' | 'supertypes')`.
