-- clice marks the code of preprocessor branches not taken with the
-- `inactive` semantic token modifier.
vim.api.nvim_set_hl(0, '@lsp.mod.inactive', { link = 'Comment', default = true })
