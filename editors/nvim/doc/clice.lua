-- Should be placed under config_dir/lsp/
-- Lsp configuration for nvim >= 0.11

local clice = {
    filetypes = { 'c', 'cpp' },

    root_markers = {
        '.git/',
        'clice.toml',
        '.clang-tidy',
        '.clang-format',
        'compile_commands.json',
        'compile_flags.txt',
        'configure.ac', -- AutoTools
    },

    capabilities = {
        textDocument = {
            completion = {
                editsNearCursor = true,
            },
        },
        offsetEncoding = { 'utf-8' },
    },

    cmd = {
        'clice',
        '--mode=pipe',
    },
}

return clice
