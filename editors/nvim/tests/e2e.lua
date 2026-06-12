-- Headless E2E smoke test for clice in neovim.
--
-- Usage: nvim -l editors/nvim/tests/e2e.lua <clice-executable> <fixture-dir>
--
-- Starts clice through the shipped LSP config (../doc/clice.lua), opens
-- the fixture's main file, waits for diagnostics, then checks that
-- hover, definition and completion respond with well-formed results.

local clice_path = arg[1]
local fixture_dir = arg[2]

local function fail(msg)
    io.stderr:write('FAIL: ' .. msg .. '\n')
    vim.cmd 'cquit 1'
end

local function step(msg)
    io.stdout:write('--- ' .. msg .. '\n')
end

if not clice_path or not fixture_dir then
    fail 'usage: nvim -l e2e.lua <clice-executable> <fixture-dir>'
end

clice_path = vim.fn.fnamemodify(clice_path, ':p')
fixture_dir = vim.fn.fnamemodify(fixture_dir, ':p'):gsub('/$', '')

if vim.fn.executable(clice_path) ~= 1 then
    fail('clice executable not found: ' .. clice_path)
end

-- Per-fixture scenario: which file to open, where to issue requests and
-- which file the definition request must land in.
-- Keep in sync with editors/vscode/src/test/e2e.test.ts.
local scenarios = {
    hello_world = {
        file = 'main.cpp',
        symbol = 'add(1, 2)',
        index_symbol = 'add',
        definition_file = 'main.cpp',
    },
    hover_on_imported_symbol = {
        file = 'use.cpp',
        symbol = 'magic_number()',
        index_symbol = 'magic_number',
        definition_file = 'defs.cppm',
    },
}

local scenario = scenarios[vim.fs.basename(fixture_dir)]
if not scenario then
    fail('no scenario for fixture: ' .. fixture_dir)
end

local plugin_root = vim.fn.fnamemodify(arg[0], ':p:h:h')
local config = dofile(plugin_root .. '/doc/clice.lua')
config.name = 'clice'
config.cmd = { clice_path, 'server' }
config.root_dir = fixture_dir

local main_file_uri = vim.uri_from_fname(fixture_dir .. '/' .. scenario.file)
local got_diagnostics = false
config.handlers = {
    ['textDocument/publishDiagnostics'] = function(_, result)
        if result and result.uri == main_file_uri then
            got_diagnostics = true
        end
    end,
}

step('open ' .. scenario.file)
vim.cmd.edit(fixture_dir .. '/' .. scenario.file)
local buf = vim.api.nvim_get_current_buf()
vim.bo[buf].filetype = 'cpp'

step 'start clice'
local client_id = vim.lsp.start(config, { bufnr = buf })
if not client_id then
    fail 'vim.lsp.start did not start clice'
end

if not vim.wait(30000, function()
    return vim.lsp.buf_is_attached(buf, client_id)
end, 100) then
    fail 'client did not attach within 30s'
end
step 'client attached'

-- First diagnostics signal that the file finished compiling; modules
-- fixtures build their dependencies first, so allow a generous timeout.
if not vim.wait(180000, function()
    return got_diagnostics
end, 200) then
    fail 'no diagnostics received within 180s'
end
step 'diagnostics received'

local function find_position(pattern)
    local lines = vim.api.nvim_buf_get_lines(buf, 0, -1, false)
    for i, line in ipairs(lines) do
        local col = line:find(pattern, 1, true)
        if col then
            return { line = i - 1, character = col - 1 }
        end
    end
    fail('pattern not found in buffer: ' .. pattern)
end

local function request(method, params)
    local responses = vim.lsp.buf_request_sync(buf, method, params, 30000)
    if not responses then
        fail(method .. ' timed out')
    end
    local response = responses[client_id]
    if not response then
        fail(method .. ' returned no response from clice')
    end
    if response.err then
        fail(method .. ' returned error: ' .. vim.inspect(response.err))
    end
    return response.result
end

-- Definition is index-based: poll workspace/symbol until the expected
-- symbol shows up, mirroring wait_for_index in the integration tests.
step 'wait for index'
local indexed = vim.wait(60000, function()
    local responses = vim.lsp.buf_request_sync(buf, 'workspace/symbol', {
        query = scenario.index_symbol,
    }, 5000)
    local result = responses and responses[client_id] and responses[client_id].result
    return result and vim.iter(result):any(function(symbol)
        return symbol.name == scenario.index_symbol
    end) or false
end, 1000)
if not indexed then
    fail('symbol ' .. scenario.index_symbol .. ' not indexed within 60s')
end

local position = find_position(scenario.symbol)
local text_document = { uri = vim.uri_from_bufnr(buf) }

step 'hover'
local hover = request('textDocument/hover', {
    textDocument = text_document,
    position = position,
})
if not hover or not hover.contents then
    fail 'hover returned no contents'
end

step 'definition'
local definition = request('textDocument/definition', {
    textDocument = text_document,
    position = position,
})
if definition and definition.uri then
    definition = { definition }
end
if not definition or #definition == 0 then
    fail 'definition returned no locations'
end
local target = definition[1].uri or definition[1].targetUri
local target_file = vim.fs.basename(vim.uri_to_fname(target))
if target_file ~= scenario.definition_file then
    fail('definition landed in ' .. target_file .. ', expected ' .. scenario.definition_file)
end

step 'completion'
local completion = request('textDocument/completion', {
    textDocument = text_document,
    position = { line = position.line, character = position.character + 1 },
    context = { triggerKind = 1 },
})
local items = completion and (completion.items or completion)
if not items or #items == 0 then
    fail 'completion returned no items'
end

step 'shutdown'
vim.lsp.get_client_by_id(client_id):stop()
if not vim.wait(15000, function()
    return vim.lsp.get_client_by_id(client_id) == nil
end, 100) then
    fail 'client did not stop within 15s'
end

io.stdout:write('PASS: ' .. vim.fs.basename(fixture_dir) .. '\n')
