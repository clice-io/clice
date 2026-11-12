-- Headless end-to-end test of clice in Neovim, set up the way a user sets it
-- up: this directory on 'runtimepath' and `vim.lsp.enable('clice')`.
--
-- Usage: nvim -l editors/nvim/tests/e2e.lua <clice-executable> <fixtures-dir> [<fixture>]
--
-- The fixtures are copies of tests/editors/NN_name (pixi run -e editor
-- editor-prepare nvim): each a project with the test hooks on, so the script
-- waits for the server through clice/internal/sync, and its scenario.json,
-- which says what to check (tools/testing/editors/scenario.ts). Each fixture
-- runs in a Neovim of its own; naming one runs it alone. Every vim.wait bound
-- only fails a hang.

local script = vim.fn.fnamemodify(arg[0], ':p')
local clice_path, fixtures_dir, fixture_name = arg[1], arg[2], arg[3]

local function fail(msg)
    io.stderr:write('FAIL: ' .. msg .. '\n')
    vim.cmd 'cquit 1'
end

local function step(msg)
    io.stdout:write('--- ' .. msg .. '\n')
end

if not clice_path or not fixtures_dir then
    fail 'usage: nvim -l e2e.lua <clice-executable> <fixtures-dir> [<fixture>]'
end

clice_path = vim.fn.fnamemodify(clice_path, ':p')
if vim.fn.executable(clice_path) ~= 1 then
    fail('clice executable not found: ' .. clice_path)
end
fixtures_dir = vim.uv.fs_realpath(fixtures_dir) or fail('no fixtures directory ' .. fixtures_dir)

if not fixture_name then
    local fixtures = {}
    for name, kind in vim.fs.dir(fixtures_dir) do
        if kind == 'directory' and vim.uv.fs_stat(fixtures_dir .. '/' .. name .. '/scenario.json') then
            table.insert(fixtures, name)
        end
    end
    table.sort(fixtures)
    if #fixtures == 0 then
        fail('no fixtures under ' .. fixtures_dir)
    end
    local failed = {}
    for _, name in ipairs(fixtures) do
        io.stdout:write('=== ' .. name .. '\n')
        local forward = function(stream)
            return function(_, data)
                if data then
                    stream:write(data)
                end
            end
        end
        local run = vim.system({ vim.v.progpath, '-l', script, clice_path, fixtures_dir, name }, {
            stdout = forward(io.stdout),
            stderr = forward(io.stderr),
        }):wait()
        if run.code ~= 0 then
            table.insert(failed, name)
        end
    end
    if #failed > 0 then
        fail('fixtures failed: ' .. table.concat(failed, ', '))
    end
    io.stdout:write(('PASS: %d fixtures\n'):format(#fixtures))
    return
end

-- `:lsp restart` restarts clice when it asks to: on a saved clice.toml, on a
-- switched build configuration.
if vim.fn.exists ':lsp' ~= 2 then
    fail 'this Neovim has no :lsp, which restarts clice when it asks to: Neovim 0.12 or later'
end

local fixture_dir = fixtures_dir .. '/' .. fixture_name
local scenario = vim.json.decode(table.concat(vim.fn.readfile(fixture_dir .. '/scenario.json'), '\n'))

vim.opt.runtimepath:append(vim.fn.fnamemodify(script, ':h:h'))
vim.cmd 'filetype on'
vim.cmd.runtime 'plugin/clice.lua'
vim.lsp.config('clice', { cmd = { clice_path, 'serve' } })
vim.lsp.enable 'clice'

local notifications = {}
vim.notify = function(msg)
    io.stdout:write('notify: ' .. msg .. '\n')
    table.insert(notifications, msg)
end
vim.ui.select = function(_, opts)
    fail('asked to choose: ' .. opts.prompt)
end

-- Pushed or pulled (a Neovim declaring pull support pulls them), the
-- diagnostics land through vim.diagnostic.set, empty ones included.
local diagnosed = {}
vim.api.nvim_create_autocmd('DiagnosticChanged', {
    callback = function(args)
        diagnosed[args.buf] = true
    end,
})

local function wait(ms, condition, what)
    if not vim.wait(ms, condition, 50) then
        fail(('no %s within %ds'):format(what, ms / 1000))
    end
end

local function relative(name)
    return vim.fs.relpath(fixture_dir, name) or name
end

local function client_of(buf)
    local client = vim.lsp.get_clients({ bufnr = buf, name = 'clice' })[1]
    return client and client.initialized and client or nil
end

local function open(file)
    vim.cmd.edit(vim.fn.fnameescape(fixture_dir .. '/' .. file))
    local buf = vim.api.nvim_get_current_buf()
    wait(60000, function()
        return client_of(buf) ~= nil
    end, 'clice attached to ' .. file)
    return buf
end

local function request(buf, method, params, timeout)
    local client = client_of(buf) or fail('clice is not attached to ' .. vim.api.nvim_buf_get_name(buf))
    local response, err = client:request_sync(method, params, timeout or 60000, buf)
    if not response then
        fail(method .. ' got no answer: ' .. tostring(err))
    elseif response.err then
        fail(method .. ' returned error: ' .. vim.inspect(response.err))
    end
    return response.result
end

-- Answers once the server has nothing left to do; its own deadline is four
-- minutes.
local function sync(buf)
    local result = request(buf, 'clice/internal/sync', vim.empty_dict(), 300000)
    if #result.pending > 0 then
        fail('the server is still busy: ' .. table.concat(result.pending, '; '))
    end
end

local function position_of(buf, text)
    for i, line in ipairs(vim.api.nvim_buf_get_lines(buf, 0, -1, false)) do
        local col = line:find(text, 1, true)
        if col then
            return { line = i - 1, character = col - 1 }
        end
    end
    fail(text .. ' is not in ' .. vim.api.nvim_buf_get_name(buf))
end

local function no_errors(buf)
    local errors = vim.diagnostic.get(buf, { severity = vim.diagnostic.severity.ERROR })
    if #errors > 0 then
        local messages = vim.tbl_map(function(diagnostic)
            return ('%d: %s'):format(diagnostic.lnum + 1, diagnostic.message)
        end, errors)
        fail(relative(vim.api.nvim_buf_get_name(buf)) .. ' has errors:\n' .. table.concat(messages, '\n'))
    end
end

-- What `command` makes the plugin tell the user next.
local function told(command, what)
    local seen = #notifications
    command()
    wait(60000, function()
        return #notifications > seen
    end, what)
    return notifications[seen + 1]
end

-- A command of the plugin ending in a request of its own: once that is
-- answered, a sync waits out the work it started.
local function answered(buf, method, command)
    local done = false
    local autocmd = vim.api.nvim_create_autocmd('LspRequest', {
        callback = function(args)
            local sent = args.data.request
            if sent.method == method and sent.type == 'complete' then
                done = true
            end
        end,
    })
    command()
    wait(60000, function()
        return done
    end, 'answer to ' .. method)
    vim.api.nvim_del_autocmd(autocmd)
    sync(buf)
end

-- Waits for the clice that replaces `before` on `buf`.
local function restarted(buf, before, what)
    wait(60000, function()
        local clients = vim.lsp.get_clients { bufnr = buf, name = 'clice' }
        return #clients == 1 and clients[1].id ~= before and clients[1].initialized
    end, what)
end

step('open ' .. scenario.file)
local buf = open(scenario.file)
local uri = vim.uri_from_bufnr(buf)
-- The first diagnostics come once the file compiled; a modules fixture builds
-- its dependencies first.
wait(180000, function()
    return diagnosed[buf]
end, 'diagnostics of ' .. scenario.file)

-- Definition is index-based.
step 'wait for the index'
sync(buf)
no_errors(buf)
local symbols = request(buf, 'workspace/symbol', { query = scenario.indexed:match '^[^(]+' })
if not vim.iter(symbols or {}):any(function(symbol)
    return symbol.name == scenario.indexed
end) then
    fail('symbol ' .. scenario.indexed .. ' not indexed')
end

local position = position_of(buf, scenario.symbol)
local text_document = { uri = uri }

step 'hover'
local hover = request(buf, 'textDocument/hover', { textDocument = text_document, position = position })
if not hover or not hover.contents then
    fail 'hover returned no contents'
end

if scenario.definition then
    step 'definition'
    local definition = request(buf, 'textDocument/definition', {
        textDocument = text_document,
        position = position,
    })
    if definition and definition.uri then
        definition = { definition }
    end
    if not definition or #definition == 0 then
        fail 'definition returned no locations'
    end
    local target = relative(vim.uri_to_fname(definition[1].uri or definition[1].targetUri))
    if target ~= scenario.definition then
        fail('definition landed in ' .. target .. ', expected ' .. scenario.definition)
    end
end

step 'completion'
local completion = request(buf, 'textDocument/completion', {
    textDocument = text_document,
    position = { line = position.line, character = position.character + 1 },
    context = { triggerKind = 1 },
})
local items = completion and (completion.items or completion)
if not items or #items == 0 then
    fail 'completion returned no items'
end

local function current_context()
    return request(buf, 'clice/currentContext', text_document)
end

step 'show context'
local command = scenario.command
local shown = told(vim.cmd.LspCliceShowContext, 'answer to LspCliceShowContext')
local tail = '(picked automatically); its command: ' .. command.source
if command.from then
    tail = tail .. ' from ' .. vim.fn.fnamemodify(fixture_dir .. '/' .. command.from, ':~:.')
end
if not vim.endswith(shown, tail) then
    fail('LspCliceShowContext showed: ' .. shown .. '\nnot ending in: ' .. tail)
end

if scenario.contexts then
    local contexts = scenario.contexts
    local function context_is(file, automatic)
        local current = current_context()
        return current.automatic == automatic and type(current.context) == 'table' and relative(vim.uri_to_fname(current.context.uri)) == file
    end
    if not context_is(contexts.automatic, true) then
        fail('the automatic context is not ' .. contexts.automatic .. ': ' .. vim.inspect(current_context()))
    end
    local listed = request(buf, 'clice/queryContext', { uri = uri })
    if listed.total ~= contexts.total then
        fail(('%d contexts listed, expected %d'):format(listed.total, contexts.total))
    end

    -- The plugin reads every page of the listing before it offers them.
    step('switch context to ' .. contexts.other)
    vim.ui.select = function(offered, _, on_choice)
        on_choice(vim.iter(offered):find(function(item)
            return item.uri and relative(vim.uri_to_fname(item.uri)) == contexts.other
        end))
    end
    answered(buf, 'clice/switchContext', vim.cmd.LspCliceSwitchContext)
    if not context_is(contexts.other, false) then
        fail('LspCliceSwitchContext did not switch to ' .. contexts.other)
    end
    shown = told(vim.cmd.LspCliceShowContext, 'answer to LspCliceShowContext')
    if not shown:find('(chosen)', 1, true) then
        fail('LspCliceShowContext showed: ' .. shown)
    end

    step 'reset context'
    answered(buf, 'clice/resetContext', vim.cmd.LspCliceResetContext)
    if not context_is(contexts.automatic, true) then
        fail 'LspCliceResetContext did not reset the context'
    end
end

if scenario.counterpart == vim.NIL then
    step 'no counterpart'
    local said = told(vim.cmd.LspCliceSwitchSourceHeader, 'answer to LspCliceSwitchSourceHeader')
    if said ~= 'clice: no counterpart of this file found' or vim.api.nvim_get_current_buf() ~= buf then
        fail('LspCliceSwitchSourceHeader without a counterpart: ' .. said)
    end
elseif scenario.counterpart then
    step('switch to ' .. scenario.counterpart)
    vim.cmd.LspCliceSwitchSourceHeader()
    wait(60000, function()
        return relative(vim.api.nvim_buf_get_name(0)) == scenario.counterpart
    end, scenario.counterpart .. ' opened by LspCliceSwitchSourceHeader')
    local counterpart = vim.api.nvim_get_current_buf()
    wait(60000, function()
        return client_of(counterpart) ~= nil
    end, 'clice attached to ' .. scenario.counterpart)
    step('switch back to ' .. scenario.file)
    vim.cmd.LspCliceSwitchSourceHeader()
    wait(60000, function()
        return vim.api.nvim_get_current_buf() == buf
    end, scenario.file .. ' opened by LspCliceSwitchSourceHeader')
end

if scenario.refactor then
    local refactor = scenario.refactor
    step('refactor ' .. refactor.file)
    local target = open(refactor.file)
    local at = position_of(target, refactor.at)
    vim.api.nvim_win_set_cursor(0, { at.line + 1, at.character })
    vim.cmd('LspCliceRefactor ' .. refactor.kind)
    wait(60000, function()
        local text = table.concat(vim.api.nvim_buf_get_lines(target, 0, -1, false), '\n')
        return text:find(refactor.adds, 1, true) ~= nil
    end, 'edit of ' .. refactor.kind)
    vim.cmd 'edit!'
    vim.cmd.buffer(buf)
end

if scenario.configurations then
    local configurations = scenario.configurations
    local function active()
        return request(buf, 'clice/listConfigurations', { uri = uri }).active
    end
    if active() ~= configurations.active then
        fail('the active configuration is ' .. active() .. ', expected ' .. configurations.active)
    end
    step('switch configuration to ' .. configurations.switch)
    vim.ui.select = function(offered, _, on_choice)
        on_choice(vim.iter(offered):find(function(name)
            return name == configurations.switch
        end))
    end
    local before = client_of(buf).id
    vim.cmd.LspCliceSwitchConfiguration()
    restarted(buf, before, 'restart under ' .. configurations.switch)
    sync(buf)
    if active() ~= configurations.switch then
        fail('the restarted clice runs ' .. active() .. ', expected ' .. configurations.switch)
    end
end

-- A saved clice.toml makes clice ask to be restarted, which the config does
-- through :lsp restart: another client attaches to the buffer.
step 'save clice.toml'
local before = client_of(buf).id
local file = assert(io.open(fixture_dir .. '/clice.toml', 'a'))
file:write '\n# edited\n'
file:close()
restarted(buf, before, 'restart after clice.toml changed')

step 'shutdown'
for _, client in ipairs(vim.lsp.get_clients { name = 'clice' }) do
    client:stop()
end
wait(60000, function()
    return #vim.lsp.get_clients { name = 'clice' } == 0
end, 'stop of clice')

io.stdout:write('PASS: ' .. fixture_name .. '\n')
