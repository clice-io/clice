-- Headless E2E smoke test for clice in neovim.
--
-- Usage: nvim -l editors/nvim/tests/e2e.lua <clice-executable> <fixture-dir>
--
-- The fixtures are copies of cases (pixi run -e editor editor-prepare), with
-- the test hooks on: the script waits for the server through
-- clice/internal/sync. Every vim.wait bound only fails a hang.
-- Starts clice through the shipped LSP config (../lsp/clice.lua), opens
-- the fixture's main.cpp, waits for diagnostics, then checks that
-- hover, definition and completion respond with well-formed results, and
-- on a header that the compilation context commands work.

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
        index_symbol = 'add(int a, int b)',
        definition_file = 'main.cpp',
    },
    imported_symbol = {
        file = 'main.cpp',
        symbol = 'magic_number()',
        index_symbol = 'magic_number()',
        definition_file = 'defs.cppm',
    },
    two_hosts = {
        file = 'shared.h',
        symbol = 'VALUE_TYPE',
        index_symbol = 'get_value()',
        context = 'int_host.cpp',
    },
}

local scenario = scenarios[vim.fs.basename(fixture_dir)]
if not scenario then
    fail('no scenario for fixture: ' .. fixture_dir)
end

local plugin_root = vim.fn.fnamemodify(arg[0], ':p:h:h')
local config = dofile(plugin_root .. '/lsp/clice.lua')
config.name = 'clice'
config.cmd = { clice_path, 'serve' }
config.root_dir = fixture_dir

step('open ' .. scenario.file)
vim.cmd.edit(fixture_dir .. '/' .. scenario.file)
local buf = vim.api.nvim_get_current_buf()
vim.bo[buf].filetype = 'cpp'

-- Pushed or pulled (a Neovim declaring pull support pulls them), the
-- diagnostics land through vim.diagnostic.set, empty ones included.
local got_diagnostics = false
vim.api.nvim_create_autocmd('DiagnosticChanged', {
    buffer = buf,
    callback = function()
        got_diagnostics = true
    end,
})

step 'start clice'
local client_id = vim.lsp.start(config, { bufnr = buf })
if not client_id then
    fail 'vim.lsp.start did not start clice'
end

if not vim.wait(60000, function()
    return vim.lsp.buf_is_attached(buf, client_id)
end, 100) then
    fail 'client did not attach within 60s'
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

local function request(method, params, timeout)
    local responses = vim.lsp.buf_request_sync(buf, method, params, timeout or 60000)
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

-- Answers once the server has nothing left to do; its own deadline is four
-- minutes.
local function sync()
    local result = request('clice/internal/sync', vim.empty_dict(), 300000)
    if #result.pending > 0 then
        fail('the server is still busy: ' .. table.concat(result.pending, '; '))
    end
end

-- Definition is index-based.
step 'wait for index'
sync()
local symbols = request('workspace/symbol', { query = scenario.index_symbol:match '^[^(]+' })
if not vim.iter(symbols or {}):any(function(symbol)
    return symbol.name == scenario.index_symbol
end) then
    fail('symbol ' .. scenario.index_symbol .. ' not indexed')
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

if scenario.definition_file then
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

if scenario.context then
    -- A context command ends in a request of the plugin's own: once it is
    -- answered, a sync waits out the compile it started.
    local function after_request(method, command)
        local answered = false
        local autocmd = vim.api.nvim_create_autocmd('LspRequest', {
            callback = function(args)
                local sent = args.data.request
                if sent.method == method and sent.type == 'complete' then
                    answered = true
                end
            end,
        })
        command()
        if not vim.wait(60000, function()
            return answered
        end, 50) then
            fail(method .. ' was not answered')
        end
        vim.api.nvim_del_autocmd(autocmd)
        sync()
    end

    local function context_is(automatic)
        local current = request('clice/currentContext', text_document)
        return current.automatic == automatic
            and type(current.context) == 'table'
            and vim.fs.basename(vim.uri_to_fname(current.context.uri)) == scenario.context
    end

    local notified
    vim.notify = function(msg)
        io.stdout:write('notify: ' .. msg .. '\n')
        notified = msg
    end
    step 'show context'
    vim.cmd.LspCliceShowContext()
    if not vim.wait(60000, function()
        return notified ~= nil
    end, 50) then
        fail 'LspCliceShowContext showed nothing'
    end
    if not notified:find(scenario.context .. '.*picked automatically.*its command: host from ' .. scenario.context) then
        fail('LspCliceShowContext showed: ' .. notified)
    end

    step 'switch context'
    vim.ui.select = function(items, opts, on_choice)
        on_choice(vim.iter(items):find(function(item)
            return opts.format_item(item):find(scenario.context, 1, true) ~= nil
        end))
    end
    after_request('clice/switchContext', vim.cmd.LspCliceSwitchContext)
    if not context_is(false) then
        fail 'LspCliceSwitchContext did not switch the context'
    end

    step 'reset context'
    after_request('clice/resetContext', vim.cmd.LspCliceResetContext)
    if not context_is(true) then
        fail 'LspCliceResetContext did not reset the context'
    end
end

step 'shutdown'
vim.lsp.get_client_by_id(client_id):stop()
if not vim.wait(60000, function()
    return vim.lsp.get_client_by_id(client_id) == nil
end, 100) then
    fail 'client did not stop within 60s'
end

io.stdout:write('PASS: ' .. vim.fs.basename(fixture_dir) .. '\n')
