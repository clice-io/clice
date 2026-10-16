-- clice names its semantic token kinds and modifiers itself: link the ones
-- Neovim has no default for to the tree-sitter groups coloring the same
-- things. `default = true` keeps a link a colorscheme or the user set.
local links = {
    ['@lsp.type.character'] = '@character',
    ['@lsp.type.escape'] = '@string.escape',
    ['@lsp.type.formatSpecifier'] = '@string.special',
    ['@lsp.type.primitive'] = '@type.builtin',
    ['@lsp.type.bracket'] = '@punctuation.bracket',
    ['@lsp.type.directive'] = '@keyword.directive',
    ['@lsp.type.header'] = '@string.special.path',
    ['@lsp.type.module'] = '@module',
    ['@lsp.type.union'] = '@type',
    ['@lsp.type.typeAlias'] = '@type.definition',
    ['@lsp.type.templateParameter'] = '@constant',
    ['@lsp.type.concept'] = '@type',
    ['@lsp.type.field'] = '@variable.member',
    ['@lsp.type.label'] = '@label',
    ['@lsp.type.macroParameter'] = '@variable.parameter',
    ['@lsp.type.attribute'] = '@attribute',
    ['@lsp.typemod.keyword.controlFlow'] = '@keyword.conditional',
    ['@lsp.typemod.keyword.literal'] = '@boolean',
    ['@lsp.typemod.keyword.documentation'] = '@comment.note',
    ['@lsp.typemod.comment.documentation'] = '@comment.documentation',
    ['@lsp.typemod.macro.callable'] = '@function.macro',
    ['@lsp.typemod.function.defaultLibrary'] = '@function.builtin',
    ['@lsp.typemod.variable.readonly'] = '@constant',
    -- Code in preprocessor branches not taken.
    ['@lsp.mod.inactive'] = 'Comment',
}
for group, target in pairs(links) do
    vim.api.nvim_set_hl(0, group, { link = target, default = true })
end
