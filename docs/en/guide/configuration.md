---
outline: 2
---

# Configuration Reference

This page lists every option; the [Configuration Guide](./config-guide.md) walks through the common cases.

clice reads configuration from three files, each laid over the one before: the user's file, `clice/config.toml` in the user's configuration directory — `$XDG_CONFIG_HOME` wherever it is set, else `~/.config` on Linux and macOS and `%APPDATA%` on Windows — which applies to every project; the project's `clice.toml` in its root, or `.clice/config.toml` if the former does not exist; and the project's local file `.clice/local.toml`, a developer's own settings beside a committed `clice.toml`, which the default cache directory's `.gitignore` keeps out of Git (with `cache_dir` set elsewhere, ignore it yourself). Configuration can also be passed via LSP `initializationOptions` (JSON format), laid over all three. A later layer's value replaces an earlier one's, a table's keys each on their own, while a list and a table of names (`[semantic_tokens.kinds]`) are replaced whole. The rules add up instead: the local file's come first, then the project's, then the user's, so that the more specific file's rules decide a field first, while their `append` and `remove` edit the command after the others', so that its flags come last; rules given in `initializationOptions` replace those of every file. Defaults fill in whatever remains unset after the merge. `clice inspect config` lists the files a project reads and each setting they change from its default, with the file and line setting it; `clice command <file>` names the rule deciding each setting of a file.

Configuration is read once, when the server starts; there is no hot reload. When a project's configuration file, its local file or the user's file is saved, created or deleted, the server asks the editor to restart it: VS Code (unless it connects to a server over a socket) and Neovim 0.12 or later restart it on their own; other editors show a diagnostic on the file, or a message for a deleted one, saying the change applies once clice restarts.

A configuration clice cannot honor is an error: a file that does not parse, a value of the wrong type, a value outside an option's choices, a worker count or a limit of zero, a pattern that does not compile or holds a backslash, a variable that is not set, a `default_configuration` no rule's tag names, a key of the layout before clice 1.0 (the error names where it went). The server shows each problem as an error diagnostic on the file setting the value — a rule's problem on the file the rule is written in — and serves nothing of that project until the fixed file is saved, nothing of any project for a problem of the user's file; a command-line run names each problem with its place and exits with code 2. A key clice does not know is a warning on the file, and the rest applies. An error in the settings from `initializationOptions` is shown as a message, and nothing of the project is served until they are fixed and clice restarts.

A JSON schema of the whole configuration is published at [`clice-config.schema.json`](/clice-config.schema.json); editors that validate TOML or JSON against a schema can point at it. The VS Code extension associates it with every configuration file for [Even Better TOML](https://marketplace.visualstudio.com/items?itemName=tamasfe.even-better-toml), which then completes and checks them; elsewhere a first line `#:schema https://docs.clice.io/clice-config.schema.json` points taplo, the TOML language server of Zed and Neovim setups alike, at it.

Relative paths and patterns resolve against the project root, whichever file or `initializationOptions` they are written in: in the user's file, they name a place in each project.

Settings come in two kinds. What applies to the whole server — its cache and log directories, how often it looks at the disk, how many workers it runs — is a top-level key or sits in `[workers]`. What can differ from one file to another — whether the background index compiles it, whether its function bodies are parsed and indexed, how it is served, linted and formatted — is a field of `[[rules]]`: to set it for every file, write it on a last rule without `patterns`. The feature sections (`[hover]`, `[inlay_hints]`, …) hold the preferences of each feature.

## Several Folders

Each workspace folder the editor opens is a project of its own, with its own `clice.toml`, compilation databases and cache directory, as long as it holds a configuration file (`clice.toml`, `.clice/config.toml` or `.clice/local.toml`) or a `compile_commands.json` (in the folder itself or one of its immediate subdirectories without a configuration file of their own, where discovery looks). A folder with neither that sits inside another open folder is part of that folder's project instead. Inside a folder, a directory holding its own `clice.toml`, `.clice/config.toml` or `.clice/local.toml` is a project of its own: the folder's project loads no database in it and takes none of its files as default sources or provisional members, though a database of the folder's own that lists them still compiles them there. A directory holding only a database serves, as a project of its own, the files the folder's project does not build. A file opened outside every folder belongs to the nearest directory above it holding a configuration file or a `compile_commands.json` (directly or in its `build/` directory), which is then served as if it were open, unless a served project loads that database already; a file with no such directory above it that no project compiles is served apart, under the default command, taking nothing from the folders' projects. Such files keep their cache in clice's temporary directory, removed when the server exits, and a rename from one of them is made when its edits stay in that file. A file is compiled by the project whose database lists it; a header without an entry of its own borrows a host from a project whose sources include it, preferring the project whose folder holds it. Workspace symbol search spans every project, and navigation — references, definitions and declarations, call and type hierarchies, implementations — reaches into the projects that declare the symbol in the same file. A file open in the editor answers only through the project serving it, so its unsaved edits are what every query sees.

The user's file and `initializationOptions` apply to every project: a rule in either naming databases or a default command turns the discovery of databases off in every project, as one in `clice.toml` does in its own. A cache directory serves one project: a directory outside the project records the project that first used it, and another project, in this server or a later `clice index` run alike, falls back to the one its `clice.toml` or local file names, then to `.clice` in its folder; the user is told where its cache went, and a project finding those taken too serves nothing until its `cache_dir` names a directory of its own. An absolute `cache_dir` in either therefore goes to the first folder only, unless it takes `${workspace_id}`: `cache_dir = "~/Library/Caches/clice/${workspace_id}"` gives each project a directory of its own. The worker counts are the largest any folder asks for at startup. Listing or switching configurations acts on the project of the file the request names — in VS Code, the active editor's — else on the first folder.

## Logs

Each run of `clice serve`, `clice index` and `clice lint` logs into a directory of its own under `logging_dir` (`.clice/logs` in the project by default), named for when it started and its process id: a server's `master.log`, or a command's `index.log` or `lint.log`, beside one log per worker. A server starts its log with the first project it opens — a folder without a compilation database waits until a file of it is opened or a database appears in it — and every other project it opens links to it from its own logging directory. A run removes all but the 20 latest sessions, links among them, never one whose process still runs. A log file past 64 MiB moves to `<name>.1.log`, replacing the one there, and the new file starts by saying so.

The logs keep `info` and above whatever the log level; `CLICE_LOG_LEVEL` in the environment (`trace`, `debug`, `info`, `warn`, `error`, `fatal` or `off`) sets what reaches stderr. An editor shows the server's, from `info` (the Neovim configuration asks for `warn`); a command prints warnings and errors as `warning: …` and `error: …`. With `CLICE_LSP_TRACE=1`, a server an editor starts also writes every LSP message it receives (`<-`) and sends (`->`), with the time, to `trace.log` in its session directory, which moves aside past 64 MiB as a log does. An editor passes these variables to the server it starts (VS Code: `clice.serverEnv`). Paths in a project are written relative to its root; a server serving several projects writes them relative to the directory holding them all, the folder's name first; a worker's own log writes them in full. To report a problem, attach the session directory: `master.log` says what the server did and how long it took, and a crash's stack trace is in the log of the process that crashed; `master.log` quotes a worker's, with the last lines the worker logged.

## Variable Substitution

The following variables are supported in `cache_dir`, `logging_dir`, and a rule's `patterns`, `compile_commands`, `default_command`, `append` and `remove`:

| Variable          | Description                                                                                                                                         |
| ----------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| `${workspace}`    | The project's root directory                                                                                                                        |
| `${workspace_id}` | A name of the project's own: its root directory's name and a hash of its path (`app-1a2b3c4d`), for a directory set for every project               |
| `${env:NAME}`     | The environment variable `NAME` of the server; one that is not set is an error. An editor passes its own environment, VS Code `clice.serverEnv` too |

In a pattern a variable's value matches literally, never as glob syntax; in a `default_command` written as one string, a value with spaces stays one argument.

## Top Level

Top-level options, written before any section.

clice looks at files on disk in the background to notice changes made outside the editor (a `git checkout`, a regenerated `compile_commands.json`, an agent or a code generator writing files), so the server picks them up without a restart. A project file is also looked at before every request that depends on it; headers the toolchain installed are looked at every few minutes, whenever their environment is updated, and at every save. Setting `poll_seconds` to `0` turns the background polling off.

The index lives in the cache directory, so it must be on a local disk clice can write: a project whose cache directory is on a network filesystem (NFS, SMB, a Windows drive under WSL) or cannot be written serves nothing, and clice says why; point `cache_dir` at a local directory and restart clice. On macOS, Time Machine and iCloud back up and sync a cache directory inside the project; a `cache_dir` under `~/Library/Caches` keeps them away.

<!-- BEGIN GENERATED CONFIG: root -->

<div class="config-option">

| Option                  | Type     | Default |
| ----------------------- | -------- | ------- |
| `default_configuration` | `string` | `""`    |

The build configuration to fall back on when neither `--configuration` nor a persisted selection names one: a tag declared on rules; a name no rule declares is an error. Unset, the first declared tag is used.

</div>

<div class="config-option">

| Option      | Type     | Default |
| ----------- | -------- | ------- |
| `cache_dir` | `string` | `""`    |

Directory for the unified on-disk cache (PCHs, PCMs and the index), relative to the project root. Empty defaults to `${workspace}/.clice`, which keeps itself out of version control and backups via generated .gitignore and CACHEDIR.TAG markers (a `.clice/config.toml` stays visible to Git, a `.clice/local.toml` does not; backup tools honoring CACHEDIR.TAG skip the whole directory); an explicitly configured directory is never marked. A cache directory serves one project: one set for every project, in the user's file or the editor's settings, takes `${workspace_id}` to give each its own. It must be a directory clice can write on a local disk: a project whose cache directory cannot be written or is on a network file system (NFS, SMB, a Windows drive under WSL) serves nothing, and clice says why. Files outside every workspace folder keep theirs in clice's temporary directory, removed when the server exits. The resolved path is printed at startup.

</div>

<div class="config-option">

| Option        | Type     | Default |
| ------------- | -------- | ------- |
| `logging_dir` | `string` | `""`    |

Directory for log files, relative to the project root; empty derives `${cache_dir}/logs`. Each server, `clice index` and `clice lint` run logs into a subdirectory of its own; the 20 latest are kept.

</div>

<div class="config-option">

| Option         | Type     | Default |
| -------------- | -------- | ------- |
| `poll_seconds` | `uint32` | `30`    |

Longest interval in seconds between two background looks at a project file: the interval doubles at every look that finds the file unchanged, up to this. 0 disables background polling, compilation databases included.

</div>

<!-- END GENERATED CONFIG -->

## `[workers]`

clice compiles in worker processes. The foreground workers keep the open files compiled and answer the requests about them; the others take one-off work — the background index, completion, signature help, formatting, precompiled headers and modules — and start as work needs them: one waits at all times, the background index starts as many as it can use when its work queues, and those idle for 30 seconds stop. How many open files keep their compiled state follows the machine's memory. A worker the system kills for lack of memory is replaced; a systemd unit running clice needs `OOMPolicy=continue`, as the default stops the whole unit, the server included, when one of its processes is killed.

<!-- BEGIN GENERATED CONFIG: workers -->

<div class="config-option">

| Option       | Type     | Default |
| ------------ | -------- | ------- |
| `foreground` | `uint32` | `2`     |

Number of foreground workers: they keep the open files compiled and answer the requests about them (hover, semantic tokens, ...); at least 1.

</div>

<div class="config-option">

| Option       | Type                 | Default  |
| ------------ | -------------------- | -------- |
| `background` | `"auto"` or `uint32` | `"auto"` |

The most compiles the background index runs at once, never more than `max`: "auto", half the machine's physical cores and at least 2, or a number, at least 1. Raising it speeds a first index up on a big machine.

</div>

<div class="config-option">

| Option | Type                 | Default  |
| ------ | -------------------- | -------- |
| `max`  | `"auto"` or `uint32` | `"auto"` |

The most compile workers at once, the background index's, completion's, signature help's and the precompiled headers' and modules' together: "auto", the machine's physical cores, or a number, at least 1. They start as work needs them and stop after 30 seconds idle.

</div>

<div class="config-option">

| Option                | Type     | Default |
| --------------------- | -------- | ------- |
| `background_delay_ms` | `uint32` | `3000`  |

How long the background index waits after the last edit or request before it starts, in milliseconds.

</div>

<!-- END GENERATED CONFIG -->

## `[diagnostics]`

The `[diagnostics]` section controls the diagnostics clice publishes for open files.

<!-- BEGIN GENERATED CONFIG: diagnostics -->

<div class="config-option">

| Option       | Type   | Default |
| ------------ | ------ | ------- |
| `clang_tidy` | `bool` | `true`  |

Run clang-tidy on open files and offer its fixes, with the checks the nearest `.clang-tidy` configures (a small default set without one). Checks too slow for an editor are left out unless `slow_clang_tidy_checks` is `true`, those unreliable on code being edited always are; files `clice lint` does not check get none. `NOLINT` comments also silence compiler warnings only while this runs.

</div>

<div class="config-option">

| Option                   | Type   | Default |
| ------------------------ | ------ | ------- |
| `slow_clang_tidy_checks` | `bool` | unset   |

Also run on open files the checks the `.clang-tidy` enables that are too slow for an editor or not measured for one (`misc-const-correctness` doubles the time a large file takes to compile): every compile of the file waits for them. Unset, clice names such checks once when it leaves them out; `false` leaves them out silently. `clice lint` runs them either way.

</div>

<div class="config-option">

| Option     | Type              | Default |
| ---------- | ----------------- | ------- |
| `suppress` | `array of string` | `[]`    |

Diagnostics open files do not show, by their code: a compiler warning's option (`-Wunused-variable`), clang's name for any other diagnostic (`err_undeclared_var_use`), a clang-tidy check, or one of clice's own warnings (`toolchain-query-failed`). For code clang gets wrong: everything else works on the file as before, and `clice lint` still reports them.

</div>

<!-- END GENERATED CONFIG -->

## `[documentation]`

The `[documentation]` section controls how clice reads documentation comments.

<!-- BEGIN GENERATED CONFIG: documentation -->

<div class="config-option">

| Option   | Type     | Default     |
| -------- | -------- | ----------- |
| `format` | `string` | `"doxygen"` |

How documentation comments are read, wherever they show (hover, completion, signature help, `clice query`): "doxygen" reads Doxygen's commands (`@brief`, `@param`, `@return`, ...) and the Markdown Doxygen allows, along with the GTK-Doc and kernel-doc conventions; "plain" shows a comment as written.

</div>

<div class="config-option">

| Option    | Type   | Default |
| --------- | ------ | ------- |
| `inherit` | `bool` | `true`  |

An overriding method without a documentation comment shows the one of the method it overrides.

</div>

<!-- END GENERATED CONFIG -->

## `[hover]`

The `[hover]` section controls how hover cards render.

<!-- BEGIN GENERATED CONFIG: hover -->

<div class="config-option">

| Option     | Type   | Default |
| ---------- | ------ | ------- |
| `show_aka` | `bool` | `true`  |

Show what a type alias stands for, e.g. `size_type` is `unsigned long`.

</div>

<div class="config-option">

| Option        | Type     | Default |
| ------------- | -------- | ------- |
| `max_members` | `uint32` | `20`    |

Maximum number of members the definition of a hovered class or enum lists: data members and member types, or enumerators; member functions are left out. `0` shows the declaration alone.

</div>

<div class="config-option">

| Option   | Type   | Default |
| -------- | ------ | ------- |
| `layout` | `bool` | `true`  |

Show how the compiler lays out a class or a field wherever it is hovered: size, alignment, a field's offset, and the padding.

</div>

<!-- END GENERATED CONFIG -->

## `[document_symbols]`

The `[document_symbols]` section controls what the outline of a file lists.

<!-- BEGIN GENERATED CONFIG: document_symbols -->

<div class="config-option">

| Option   | Type   | Default |
| -------- | ------ | ------- |
| `macros` | `bool` | `true`  |

List the `#define`s the file writes; an include guard's never lists.

</div>

<div class="config-option">

| Option   | Type   | Default |
| -------- | ------ | ------- |
| `locals` | `bool` | `true`  |

List what a function's body declares: its variables, types and lambdas, under the function.

</div>

<!-- END GENERATED CONFIG -->

## `[inlay_hints]`

The `[inlay_hints]` section controls which inlay hints the server produces and how long they may grow. Changes take effect after a server restart.

<!-- BEGIN GENERATED CONFIG: inlay_hints -->

<div class="config-option">

| Option    | Type   | Default |
| --------- | ------ | ------- |
| `enabled` | `bool` | `true`  |

Master switch: `false` disables all inlay hints.

</div>

<div class="config-option">

| Option       | Type   | Default |
| ------------ | ------ | ------- |
| `parameters` | `bool` | `true`  |

Parameter name hints at call sites, e.g. `draw(width: 800, height: 600)`.

</div>

<div class="config-option">

| Option               | Type   | Default |
| -------------------- | ------ | ------- |
| `mutable_references` | `bool` | `true`  |

`&` marking an argument passed by mutable reference, e.g. `mutate(&value: v)`.

</div>

<div class="config-option">

| Option           | Type   | Default |
| ---------------- | ------ | ------- |
| `prefixed_names` | `bool` | `false` |

Parameter name hints for an argument named like the parameter without its one-letter prefix, e.g. `param` passed to `aParam`.

</div>

<div class="config-option">

| Option                | Type   | Default |
| --------------------- | ------ | ------- |
| `single_letter_names` | `bool` | `false` |

Parameter name hints for a function taking one parameter whose name is a single letter, e.g. `push_back(x: 1)`.

</div>

<div class="config-option">

| Option          | Type   | Default |
| --------------- | ------ | ------- |
| `deduced_types` | `bool` | `true`  |

Deduced type hints for `auto` variables, structured bindings, init-captures and `auto` parameters, the type a `decltype` names, and deduced return types.

</div>

<div class="config-option">

| Option          | Type   | Default |
| --------------- | ------ | ------- |
| `spelled_types` | `bool` | `false` |

Deduced type hints for a variable whose initializer already names its type, e.g. `static_cast<long>(x)`, `Widget{1, 2}` or `std::make_unique<Widget>()`.

</div>

<div class="config-option">

| Option                       | Type   | Default |
| ---------------------------- | ------ | ------- |
| `deduced_template_arguments` | `bool` | `true`  |

The template arguments class template argument deduction found, e.g. `<int, double>` after `pair` in `std::pair pair(1, 2.5)`.

</div>

<div class="config-option">

| Option        | Type   | Default |
| ------------- | ------ | ------- |
| `designators` | `bool` | `true`  |

Field designator hints in aggregate initialization, e.g. `.x=` and `.y=` in `Point{1, 2}`.

</div>

<div class="config-option">

| Option          | Type   | Default |
| --------------- | ------ | ------- |
| `array_indices` | `bool` | `false` |

Array index designator hints in aggregate initialization, e.g. `[0]=` and `[1]=` in `int pair[2] = {1, 2}`.

</div>

<div class="config-option">

| Option      | Type   | Default |
| ----------- | ------ | ------- |
| `block_end` | `bool` | `false` |

`// name` after the closing brace of a function, type or control flow block, linking to its start.

</div>

<div class="config-option">

| Option          | Type   | Default |
| --------------- | ------ | ------- |
| `namespace_end` | `bool` | `true`  |

`// namespace name` after the closing brace of a namespace, linking to its start.

</div>

<div class="config-option">

| Option  | Type   | Default |
| ------- | ------ | ------- |
| `endif` | `bool` | `true`  |

`// condition` after an `#endif`, the condition of the `#if` it closes, linking to it.

</div>

<div class="config-option">

| Option              | Type   | Default |
| ------------------- | ------ | ------- |
| `default_arguments` | `bool` | `false` |

The default arguments a call omitted.

</div>

<div class="config-option">

| Option            | Type     | Default |
| ----------------- | -------- | ------- |
| `min_block_lines` | `uint32` | `10`    |

The height in lines, both ends included, from which a block, namespace or conditional gets a hint at its end.

</div>

<div class="config-option">

| Option             | Type                      | Default |
| ------------------ | ------------------------- | ------- |
| `max_length.types` | `"unlimited"` or `uint32` | `32`    |

Maximum length in characters of a deduced type or of deduced template arguments, or `"unlimited"`. The template arguments that do not fit become `…`, e.g. `tuple<int, string, …>`; a type whose outermost name does not fit is not shown.

</div>

<div class="config-option">

| Option                         | Type                      | Default |
| ------------------------------ | ------------------------- | ------- |
| `max_length.default_arguments` | `"unlimited"` or `uint32` | `32`    |

Maximum length in characters of the omitted default arguments, or `"unlimited"`. One too long becomes `…`, and the list is cut with `…` where it no longer fits.

</div>

<div class="config-option">

| Option                 | Type                      | Default |
| ---------------------- | ------------------------- | ------- |
| `max_length.block_end` | `"unlimited"` or `uint32` | `60`    |

Maximum length in characters of a block end or namespace end label, cut with `…` past it, or `"unlimited"`.

</div>

<div class="config-option">

| Option             | Type                      | Default |
| ------------------ | ------------------------- | ------- |
| `max_length.endif` | `"unlimited"` or `uint32` | `60`    |

Maximum length in characters of an `#endif` condition, cut with `…` past it, or `"unlimited"`.

</div>

<!-- END GENERATED CONFIG -->

## `[semantic_tokens]`

The `[semantic_tokens]` section controls what the semantic tokens hold and the names their kinds and modifiers are sent by. The names are announced when the editor connects, so changes take effect after a server restart.

<!-- BEGIN GENERATED CONFIG: semantic_tokens -->

<div class="config-option">

| Option    | Type   | Default |
| --------- | ------ | ------- |
| `lexical` | `bool` | `true`  |

Send the tokens an editor's own syntax highlighting colors as well: keywords, literals, comments, operators, preprocessor directives. When off, only what syntax cannot tell is sent: names by what they name, overloaded and declarator operators, template brackets, format specifiers, and the tokens of preprocessor-inactive code, kindless.

</div>

<div class="config-option">

| Option  | Type              | Default |
| ------- | ----------------- | ------- |
| `kinds` | `table of string` | `{}`    |

The names a client or theme expects for clice's token kinds, as `field = "property"`; an empty name drops the kind, whose tokens are then not sent, and kinds given one name merge. Read when the server starts.

</div>

<div class="config-option">

| Option      | Type              | Default |
| ----------- | ----------------- | ------- |
| `modifiers` | `table of string` | `{}`    |

The names a client or theme expects for clice's token modifiers; an empty name drops the modifier, and modifiers given one name merge. Read when the server starts.

</div>

<!-- END GENERATED CONFIG -->

## `[code_completion]`

The `[code_completion]` section controls completion item assembly.

<!-- BEGIN GENERATED CONFIG: code_completion -->

<div class="config-option">

| Option      | Type     | Default          |
| ----------- | -------- | ---------------- |
| `arguments` | `string` | `"placeholders"` |

What completing a call or a template inserts after the name: "placeholders" its parentheses or angle brackets with a placeholder per parameter, "delimiters" the empty parentheses or brackets with the cursor between them, "none" the name alone. It applies to functions, function-like macros, variables holding a lambda and class, alias and variable templates; an overload set listed as one item takes the delimiters. Nothing is added where the arguments are written already or the name is not called, as when taking a function's address. Clients without snippet support get the parentheses of a call alone. An Objective-C message takes a placeholder or an empty tab stop after each piece of its selector, its first piece alone with "none" or without snippet support.

</div>

<div class="config-option">

| Option             | Type   | Default |
| ------------------ | ------ | ------- |
| `bundle_overloads` | `bool` | `true`  |

Collapse an overload set into a single completion item.

</div>

<div class="config-option">

| Option               | Type   | Default |
| -------------------- | ------ | ------- |
| `statement_snippets` | `bool` | `false` |

Complete statements such as `if` and `for` as snippets with placeholders for their parts; otherwise only the keyword is inserted. Ignored for clients without snippet support.

</div>

<div class="config-option">

| Option         | Type   | Default |
| -------------- | ------ | ------- |
| `dot_to_arrow` | `bool` | `true`  |

After `.` on a pointer, list the members `->` reaches and turn the `.` into `->` when one is chosen.

</div>

<div class="config-option">

| Option       | Type   | Default |
| ------------ | ------ | ------- |
| `all_scopes` | `bool` | `true`  |

Where an unqualified name is being typed, also offer the project's names of other namespaces it matches, inserting the qualifier they need; one in several namespaces is offered once per namespace. Names the file reaches already are offered as they are; for the others see `insert_includes` and `insert_imports`. The names of other namespaces come from the index.

</div>

<div class="config-option">

| Option            | Type   | Default |
| ----------------- | ------ | ------- |
| `insert_includes` | `bool` | `true`  |

With `all_scopes`, also offer names declared, and macros defined, in headers the file does not include yet, adding the `#include` of the header that holds them.

</div>

<div class="config-option">

| Option            | Type              | Default |
| ----------------- | ----------------- | ------- |
| `angled_includes` | `array of string` | `[]`    |

Glob patterns of the headers whose `#include`, added by completion or a code action, takes angle brackets, matched against the path written between them (`fmt/core.h`) with the syntax of a rule's `patterns`: `fmt/**` matches every header below `fmt/`. A header neither this nor `quoted_includes` matches takes quotes when found next to the file or through `-iquote` or `-I`, and angle brackets when found through `-isystem`, `-idirafter` or a framework directory; one an IWYU pragma names to include instead keeps the pragma's. A header found through the toolchain's own directories, the standard library's, always takes angle brackets. A pattern is followed even where its delimiters do not find the header: angle brackets do not search the file's own directory or `-iquote`.

</div>

<div class="config-option">

| Option            | Type              | Default |
| ----------------- | ----------------- | ------- |
| `quoted_includes` | `array of string` | `[]`    |

Glob patterns of the headers whose `#include`, added by completion or a code action, takes quotes, as `angled_includes` describes. A header both match takes quotes: `angled_includes = ["**"]` with `quoted_includes = ["myproj/**"]` gives every header angle brackets but the project's own.

</div>

<div class="config-option">

| Option           | Type   | Default |
| ---------------- | ------ | ------- |
| `insert_imports` | `bool` | `true`  |

With `all_scopes`, also offer names exported by modules the file does not import yet, adding the `import`. Where the project builds the `std` module, a file that imports modules or is a module unit takes the standard library's names with `import std;` rather than their headers.

</div>

<div class="config-option">

| Option  | Type                      | Default |
| ------- | ------------------------- | ------- |
| `limit` | `"unlimited"` or `uint32` | `100`   |

Maximum number of completion items, the best ranked, or `"unlimited"`. A list cut short is marked incomplete, so the editor asks again as you type.

</div>

<!-- END GENERATED CONFIG -->

## `[signature_help]`

The `[signature_help]` section controls what the signature help popup shows.

<!-- BEGIN GENERATED CONFIG: signature_help -->

<div class="config-option">

| Option          | Type   | Default |
| --------------- | ------ | ------- |
| `documentation` | `bool` | `true`  |

Show the called function's documentation comment.

</div>

<!-- END GENERATED CONFIG -->

## `[references]`

The `[references]` section controls what find references answers.

<!-- BEGIN GENERATED CONFIG: references -->

<div class="config-option">

| Option  | Type                      | Default |
| ------- | ------------------------- | ------- |
| `limit` | `"unlimited"` or `uint32` | `2000`  |

Maximum number of locations a references request answers with, or `"unlimited"`. A reply cut short keeps the locations in the file it is asked from before any other. `clice query references` lists every one.

</div>

<!-- END GENERATED CONFIG -->

## `[[rules]]`

A rule names files by pattern and says where they take their compile commands from, how those commands are edited, and how the files are indexed, served, linted and formatted. Every rule matching a file applies, in declaration order: an earlier rule's databases rank first among the file's candidates, `append` and `remove` accumulate with a later `remove` cancelling an earlier `append`, and every other field — `default_command`, `background_index`, `readonly`, `lint`, … — is decided by the first matching rule that sets it, the default deciding when none does. A rule for some files therefore goes before a rule for all of them: a rule without `patterns` that is not last and sets a field a later rule sets too is a warning, as that later value never applies. A rule carrying a `configuration` tag applies only while that configuration is active; the distinct tags form the configuration menu. `clice command <file>` shows, for a file, the value of each field below from `query_toolchain` on, and the rule deciding it.

<!-- BEGIN GENERATED CONFIG: rules -->

<div class="config-option">

| Option     | Type              | Default |
| ---------- | ----------------- | ------- |
| `patterns` | `array of string` | `[]`    |

Glob patterns selecting the files this rule applies to. A relative pattern is anchored at the project root (`..` segments allowed); an absolute pattern or one starting with `**` matches the file's absolute path: its real path, symlinks resolved, or the path the build reaches it by, either one (Bazel's `external/...`, linked from elsewhere). `*` matches within a path segment, `?` a single character, `**` any number of segments, `{a,b}` alternatives, `[0-9]` a character range, `[!...]` a negated range, `[*]` a literal `*`. Separators are `/` on every platform. Omitted means every file: such a rule belongs last, where it decides what no rule above it does.

</div>

<div class="config-option">

| Option          | Type     | Default |
| --------------- | -------- | ------- |
| `configuration` | `string` | `""`    |

Build configuration tag. A tagged rule applies only while that configuration is active; an untagged rule always applies. The distinct tags form the configuration menu; `--configuration`, the persisted selection and `default_configuration` pick the active one, in that order.

</div>

<div class="config-option">

| Option             | Type              | Default |
| ------------------ | ----------------- | ------- |
| `compile_commands` | `array of string` | `[]`    |

Compilation databases, in priority order: a compile_commands.json or a directory containing one, relative to the project root. All of them load, and every entry applies to its own file whatever the patterns say; the patterns and the order decide which entry a file present in several databases gets by default. A rule without patterns names the project's databases. When no rule declares a source, every compile_commands.json in the project root and its immediate subdirectories loads, and so do the ones in the directories above a file when it is opened; a directory below the root with a clice.toml, .clice/config.toml or .clice/local.toml of its own is another project, and none in it loads.

</div>

<div class="config-option">

| Option             | Type   | Default |
| ------------------ | ------ | ------- |
| `resolve_symlinks` | `bool` | `false` |

Whether builds are told apart by the real paths this rule's `compile_commands` resolve to, through symlinks and Windows junctions, rather than by the configuration's name: for a build system that selects a build by pointing a link at its directory. Each real path keeps an index of its own, and when the link is pointed elsewhere the server asks to be restarted on that build's index, which still holds what an earlier session indexed there. A configuration whose rules differ from this configuration's only in naming exactly those databases directly shares its index. The index of a build no longer used stays in the cache directory until deleted.

</div>

<div class="config-option">

| Option            | Type                          | Default |
| ----------------- | ----------------------------- | ------- |
| `default_command` | `string` or `array of string` | `""`    |

The compile command for matching files without a database entry, without the source file: a string tokenized like a shell command line, or an argv array. It runs from the project root, and the matching source files on disk join the background index (unless `background_index` is `false`) — enumerated at startup and again on every poll, so a file created later compiles when opened and joins the index within a poll period. A file clang has no language for (an Arduino `.ino`, an extensionless tool) is a source only when the command forces a language (`-x c++`) and a pattern names its suffix (`*.ino`) or the file itself. Omitted means none.

</div>

<div class="config-option">

| Option   | Type              | Default |
| -------- | ----------------- | ------- |
| `append` | `array of string` | `[]`    |

Compilation flags appended for matching files, e.g. `["-std=c++20", "-DNDEBUG"]`. The flags of every matching rule apply: those of the user's file first, then the project's, then its local file's, each file's in declaration order, so that a more specific file's flags come last.

</div>

<div class="config-option">

| Option   | Type              | Default |
| -------- | ----------------- | ------- |
| `remove` | `array of string` | `[]`    |

Compilation flags removed for matching files, e.g. `["-Wall"]`, before the rule's own `append`. A flag with a value removes that value (`-DDEBUG`), the next element being a separate value as on a command line; one with `*` for its value (`-I*`, `["-include", "*"]`), or without one at the end of the list (`["-include"]`), removes every value; a `*` right after a flag's name removes every flag of its kind spelled with that prefix (`-W*` takes `-Wall` and `-Wextra` too).

</div>

<div class="config-option">

| Option            | Type   | Default |
| ----------------- | ------ | ------- |
| `query_toolchain` | `bool` | unset   |

Whether clice runs the compiler of matching files' commands to ask how it compiles them: its target, standard library and system headers, predefined macros and default language standard. `false` runs no compiler: clice's own clang reads the command as written, with clang's defaults for the compiler's name. Unset means `true`.

</div>

<div class="config-option">

| Option             | Type   | Default |
| ------------------ | ------ | ------- |
| `background_index` | `bool` | unset   |

Whether the background index compiles matching files as translation units of their own. `false` keeps them out, and drops what an earlier index recorded of them: they still compile when opened, and a header among them is still indexed through the units including it. A module unit, or the unit writing a precompiled header, is indexed all the same by the build of its module or header, for the files reading it. A rule without patterns saying `false` keeps the background index from compiling anything. Unset means `true`.

</div>

<div class="config-option">

| Option               | Type   | Default |
| -------------------- | ------ | ------- |
| `skip_function_body` | `bool` | unset   |

Whether the compiles that include matching files skip their function bodies. Unset, the editor's compile of an open file skips the bodies of the headers it includes at its top — several times faster to open and rebuild, the errors inside those bodies shown once the background index has compiled the file as saved — and every other compile reads them. `true`: every compile but a matching file's own skips its bodies, templates' included, the background index's too, and none of them is indexed — the fastest index of code you only call, such as third-party libraries, whose templates' errors your code causes are then never shown. The bodies of templates forwarding their arguments (`make_unique`) and of `constexpr` and `auto` functions are read all the same. `false`: no compile skips them, and their errors show at once.

</div>

<div class="config-option">

| Option                | Type   | Default |
| --------------------- | ------ | ------- |
| `index_function_body` | `bool` | unset   |

Whether the index records what the function bodies of matching files use, and what their templates' instantiations resolve, so that references and the call hierarchy reach inside them. Unset means `false` for system headers — a library's own internals, which only a reader of its code asks about — and `true` for every other file. A file opened in the editor is indexed whole all the same. Applies to the units indexed after a change.

</div>

<div class="config-option">

| Option     | Type     | Default |
| ---------- | -------- | ------- |
| `readonly` | `string` | unset   |

How matching files are served when open: "off" compiles them — builds are pulled by the first request that needs them, with the index answering in the meantime; "on" builds no preamble PCH — reads serve from the index alone (a cold file jumps the indexing queue), while completion and signature help still compile on demand without a preamble; "auto" starts a file as "on", switches it to "off" at the first edit intent (edit, completion, signature help, context switch), and falls back to "off" for a file the index cannot serve. Unset means "off".

</div>

<div class="config-option">

| Option           | Type   | Default |
| ---------------- | ------ | ------- |
| `self_contained` | `bool` | unset   |

Whether matching headers compile on their own. `true`: a header compiles alone, without an includer's context; `false`: always in the context of a file including it. Unset, clice compiles a header alone once to find out.

</div>

<div class="config-option">

| Option           | Type   | Default |
| ---------------- | ------ | ------- |
| `other_contexts` | `bool` | unset   |

Whether an open matching file shows, in the code its own context skips, what its other contexts compile there — the semantic colors, outline, folds, links and hover the index holds for them — while it is as the index read it. `false` shows its own context alone. The editor's choice for a file (Show or Hide other contexts) overrides it until the server stops. Unset means `true`.

</div>

<div class="config-option">

| Option | Type   | Default |
| ------ | ------ | ------- |
| `lint` | `bool` | unset   |

Whether `clice lint` checks matching files and the editor runs clang-tidy on them. `false` keeps them out: their findings are dropped and a translation unit they head is not parsed, unless `--index` needs it for the index. Files outside the project are never checked. Unset means `true`.

</div>

<div class="config-option">

| Option   | Type   | Default |
| -------- | ------ | ------- |
| `format` | `bool` | unset   |

Whether `clice format` formats matching files. `false` keeps them out. Files outside the project are never formatted. Unset means `true`.

</div>

<!-- END GENERATED CONFIG -->
