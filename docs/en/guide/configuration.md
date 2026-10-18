---
outline: 2
---

# Configuration

clice reads configuration from `clice.toml` in the workspace root, or from `.clice/config.toml` if the former does not exist. Configuration can also be passed via LSP `initializationOptions` (JSON format); values from `initializationOptions` override the config file, and defaults fill in whatever remains unset after the merge.

Configuration is read once at server startup. Changing it — either file — requires restarting the server; there is no hot reload.

A JSON schema of the whole configuration is published at [`clice-config.schema.json`](/clice-config.schema.json); editors that validate TOML or JSON against a schema can point at it.

Relative paths and patterns in the file resolve against the directory of the configuration file itself; values passed through `initializationOptions` resolve against the workspace root.

## Editor Settings

Settings an editor passes in `initializationOptions` take the place of the same settings in the file.

<!-- BEGIN GENERATED ITEMS: config/layers -->

<!-- BEGIN CAPABILITY: supported -->

**Editor settings over the file**

Rules the editor sends replace the rules of `clice.toml`

`clice.toml` gives `main.cpp` the macro it uses, and the file compiles. An editor that sends its own `rules` in its settings replaces the file's rules rather than adding to them, so here the macro is gone.

```case
tests/config/layers/01_editor_overrides
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Restarting

An edit to the configuration applies when the server starts again.

<!-- BEGIN GENERATED ITEMS: config/loading -->

<!-- BEGIN CAPABILITY: supported -->

**Changes need a restart**

clice reads `clice.toml` when the server starts, so an edit applies at the next start

The rule's macro changes while the server runs, and `main.cpp` keeps the error until the server is started again.

```case
tests/config/loading/04_edited_file
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Several Folders

Each workspace folder the editor opens is a project of its own, with its own `clice.toml`, compilation databases and cache directory, as long as it holds a `clice.toml` or a `compile_commands.json` (in the folder itself or one of its immediate subdirectories, where discovery looks). A folder with neither that sits inside another open folder is part of that folder's project instead; and inside a folder, a directory holding its own `clice.toml` or database serves, as a project of its own, the files the folder's project does not build. A file opened outside every folder belongs to the nearest directory above it holding a `clice.toml` or a `compile_commands.json` (directly or in its `build/` directory), which is then served as if it were open, unless a served project loads that database already. A file is compiled by the project whose database lists it; a header without an entry of its own borrows a host from a project whose sources include it, preferring the project whose folder holds it. Workspace symbol search spans every project, and navigation — references, definitions and declarations, call and type hierarchies, implementations — reaches into the projects that declare the symbol in the same file. A file open in the editor answers only through the project serving it, so its unsaved edits are what every query sees.

`initializationOptions` apply to every project, and a cache directory serves one project: a directory outside the project records the project that first used it, and another project, in this server or a later `clice index` run alike, falls back to the one its `clice.toml` names, then to `.clice` in its folder, and runs without a cache when those are taken too. An absolute `cache_dir` in `initializationOptions` therefore goes to the first folder only. The worker counts are the largest any folder asks for at startup. Listing or switching configurations acts on the project of the file the request names — in VS Code, the active editor's — else on the first folder.

## Variable Substitution

The following variable is supported in string values:

| Variable       | Description                                    |
| -------------- | ---------------------------------------------- |
| `${workspace}` | The workspace directory provided by the client |

## Workspace

Top-level options, written before any section.

<!-- BEGIN GENERATED CONFIG: root -->

<div class="config-option">

| Option                  | Type     | Default |
| ----------------------- | -------- | ------- |
| `default_configuration` | `string` | `""`    |

The build configuration to fall back on when neither `--configuration` nor a persisted selection names one: a tag declared on rules. When rules carry tags and this names none of them, the first declared tag is used and a warning is logged.

</div>

<!-- END GENERATED CONFIG -->

## `[project]`

<!-- BEGIN GENERATED CONFIG: project -->

<div class="config-option">

| Option      | Type     | Default |
| ----------- | -------- | ------- |
| `cache_dir` | `string` | `""`    |

Directory for the unified on-disk cache (PCH, PCM and index artifacts). Empty defaults to `${workspace}/.clice`, which keeps itself out of version control and backups via generated .gitignore and CACHEDIR.TAG markers (a `.clice/config.toml` stays visible to Git; backup tools honoring CACHEDIR.TAG skip the whole directory); an explicitly configured directory is never marked. The resolved path is printed at startup.

</div>

<div class="config-option">

| Option        | Type     | Default |
| ------------- | -------- | ------- |
| `logging_dir` | `string` | `""`    |

Directory for log files; empty derives `${cache_dir}/logs`. Each server session logs into its own timestamped subdirectory.

</div>

<div class="config-option">

| Option            | Type   | Default |
| ----------------- | ------ | ------- |
| `enable_indexing` | `bool` | `true`  |

Build the background index that serves cross-TU features (find references, workspace symbols, ...).

</div>

<div class="config-option">

| Option                            | Type   | Default |
| --------------------------------- | ------ | ------- |
| `standard_library_instantiations` | `bool` | `false` |

Index what the standard library's own template instantiations and macros resolve inside the library — the comparator `std::sort` calls, the destructor `std::vector` runs — so references and the call hierarchy reach those places. Off by default: they are library internals, and walking them costs indexing time and space. Applies to the units indexed after a change.

</div>

<div class="config-option">

| Option     | Type     | Default |
| ---------- | -------- | ------- |
| `readonly` | `string` | `"off"` |

Read-only serving for open files: "off" targets a full AST for every open file — builds are pulled by the first request that needs them, with the index answering in the meantime; "on" never builds a PCH — reads serve from the index alone (a cold file jumps the indexing queue), while completion and signature help still compile on demand without a preamble; "auto" starts every file as "on", switches it to "off" at the first edit intent (edit, completion, signature help, context switch), and falls back to "off" for a file the index cannot serve. Feature routing always answers from the best source currently available.

</div>

<div class="config-option">

| Option            | Type     | Default |
| ----------------- | -------- | ------- |
| `idle_timeout_ms` | `uint32` | `3000`  |

Idle delay in milliseconds before background indexing starts.

</div>

<div class="config-option">

| Option       | Type   | Default |
| ------------ | ------ | ------- |
| `test_hooks` | `bool` | `false` |

Enable the clice/internal test hooks used by the test harness.

</div>

<div class="config-option">

| Option                  | Type     | Default |
| ----------------------- | -------- | ------- |
| `stateful_worker_count` | `uint32` | `2`     |

Number of stateful workers — they hold ASTs in memory and serve queries (hover, semantic tokens, ...); `0` is invalid and falls back to the default.

</div>

<div class="config-option">

| Option                   | Type     | Default |
| ------------------------ | -------- | ------- |
| `stateless_worker_count` | `uint32` | —       |

Number of stateless workers started — they handle ephemeral tasks (PCH/PCM builds, completion, signature help, background indexing) — and the most that background indexing keeps busy at once; defaults to half the machine's physical cores, at least 2. `0` is invalid and falls back to that default.

</div>

<div class="config-option">

| Option                       | Type     | Default |
| ---------------------------- | -------- | ------- |
| `min_stateless_worker_count` | `uint32` | `1`     |

Lower bound for dynamic stateless-worker scaling; `0` is invalid and falls back to the default.

</div>

<div class="config-option">

| Option                       | Type     | Default |
| ---------------------------- | -------- | ------- |
| `max_stateless_worker_count` | `uint32` | —       |

Upper bound for dynamic stateless-worker scaling, which only interactive work that finds every worker busy reaches; `0` means the machine's physical cores, which is also the default.

</div>

<!-- END GENERATED CONFIG -->

## `[tracker]`

clice looks at files on disk in the background to notice changes made outside the editor (a `git checkout`, a regenerated `compile_commands.json`, an agent or a code generator writing files), so the server picks them up without a restart. A workspace file is also looked at before every request that depends on it; headers the toolchain installed are looked at every few minutes, whenever their environment is updated, and at every save. Setting `workspace_poll_seconds` to `0` turns the background polling off.

<!-- BEGIN GENERATED CONFIG: tracker -->

<div class="config-option">

| Option                   | Type     | Default |
| ------------------------ | -------- | ------- |
| `workspace_poll_seconds` | `uint32` | `30`    |

Longest interval in seconds between two background looks at a workspace file: the interval doubles at every look that finds the file unchanged, up to this. 0 disables background polling, compilation databases included.

</div>

<!-- END GENERATED CONFIG -->

## `[diagnostics]`

The `[diagnostics]` section controls the diagnostics clice publishes for open files.

<!-- BEGIN GENERATED CONFIG: diagnostics -->

<div class="config-option">

| Option       | Type   | Default |
| ------------ | ------ | ------- |
| `clang_tidy` | `bool` | `true`  |

Run clang-tidy on open files and offer its fixes, with the checks the nearest `.clang-tidy` configures (a small default set without one). Checks too slow for an editor, and those unreliable on code being edited, are left out; files `clice lint` does not check get none. `NOLINT` comments also silence compiler warnings only while this runs.

</div>

<div class="config-option">

| Option     | Type              | Default |
| ---------- | ----------------- | ------- |
| `suppress` | `array of string` | `[]`    |

Diagnostics open files do not show, by their code: a compiler warning's option (`-Wunused-variable`), clang's name for any other diagnostic (`err_undeclared_var_use`), or a clang-tidy check. For code clang gets wrong: everything else works on the file as before, and `clice lint` still reports them.

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

The `[inlay_hints]` section controls which inlay hints the server produces and how long they may grow. Changes take effect after a server restart; no recompile is involved.

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

| Option             | Type     | Default |
| ------------------ | -------- | ------- |
| `max_length.types` | `uint32` | `32`    |

Maximum length in characters of a deduced type or of deduced template arguments; `0` means no limit. The template arguments that do not fit become `…`, e.g. `tuple<int, string, …>`; a type whose outermost name does not fit is not shown.

</div>

<div class="config-option">

| Option                         | Type     | Default |
| ------------------------------ | -------- | ------- |
| `max_length.default_arguments` | `uint32` | `32`    |

Maximum length in characters of the omitted default arguments; `0` means no limit. One too long becomes `…`, and the list is cut with `…` where it no longer fits.

</div>

<div class="config-option">

| Option                 | Type     | Default |
| ---------------------- | -------- | ------- |
| `max_length.block_end` | `uint32` | `60`    |

Maximum length in characters of a block end or namespace end label, cut with `…` past it; `0` means no limit.

</div>

<div class="config-option">

| Option             | Type     | Default |
| ------------------ | -------- | ------- |
| `max_length.endif` | `uint32` | `60`    |

Maximum length in characters of an `#endif` condition, cut with `…` past it; `0` means no limit.

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

What completing a call or a template inserts after the name: "placeholders" its parentheses or angle brackets with a placeholder per parameter, "delimiters" the empty parentheses or brackets with the cursor between them, "none" the name alone. It applies to functions, function-like macros, variables holding a lambda and class, alias and variable templates; an overload set listed as one item takes the delimiters. Nothing is added where the arguments are written already or the name is not called, as when taking a function's address. Clients without snippet support get the parentheses of a call alone.

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

Where an unqualified name is being typed, also offer the project's names of other namespaces it matches, inserting the qualifier they need; one in several namespaces is offered once per namespace. Names the file reaches already are offered as they are; for the others see `insert_includes` and `insert_imports`. Needs the index for names the file does not reach.

</div>

<div class="config-option">

| Option            | Type   | Default |
| ----------------- | ------ | ------- |
| `insert_includes` | `bool` | `true`  |

With `all_scopes`, also offer names declared in headers the file does not include yet, adding the `#include` of the header that declares them.

</div>

<div class="config-option">

| Option           | Type   | Default |
| ---------------- | ------ | ------- |
| `insert_imports` | `bool` | `true`  |

With `all_scopes`, also offer names exported by modules the file does not import yet, adding the `import`.

</div>

<div class="config-option">

| Option  | Type     | Default |
| ------- | -------- | ------- |
| `limit` | `uint32` | `0`     |

Maximum number of completion items, the best ranked; `0` means no limit. A list cut short is marked incomplete, so the editor asks again as you type.

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

| Option  | Type     | Default |
| ------- | -------- | ------- |
| `limit` | `uint32` | `1000`  |

Maximum number of locations a references request answers with; `0` means no limit. A reply cut short keeps the locations in the file it is asked from before any other. `clice query references` lists every one.

</div>

<!-- END GENERATED CONFIG -->

## `[[rules]]`

A rule names files by pattern and says where they take their compile commands from and how those commands are edited. Every rule matching a file applies, in declaration order: an earlier rule's databases rank first among the file's candidates, the first matching rule with a `default_command` supplies the command of a file without an entry, `append` and `remove` accumulate with a later `remove` cancelling an earlier `append`, and `index = false` on any matching rule keeps the file out of the index. A rule carrying a `configuration` tag applies only while that configuration is active; the distinct tags form the configuration menu.

<!-- BEGIN GENERATED CONFIG: rules -->

<div class="config-option">

| Option     | Type              | Default |
| ---------- | ----------------- | ------- |
| `patterns` | `array of string` | `[]`    |

Glob patterns selecting the files this rule applies to. A relative pattern is anchored at this configuration file's directory (`..` segments allowed), or at the workspace root for a rule passed through initializationOptions; an absolute pattern or one starting with `**` matches the file's absolute path. `*` matches within a path segment, `?` a single character, `**` any number of segments, `{a,b}` alternatives, `[0-9]` a character range, `[!...]` a negated range. Omitted means every file.

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

Compilation databases, in priority order: a compile_commands.json or a directory containing one, relative to this configuration file (to the workspace root for a rule passed through initializationOptions). All of them load, and every entry applies to its own file whatever the patterns say; the patterns and the order decide which entry a file present in several databases gets by default. A rule without patterns names the workspace's databases. When no rule declares a source, every compile_commands.json in the workspace root and its immediate subdirectories loads, and so do the ones in the directories above a file when it is opened.

</div>

<div class="config-option">

| Option            | Type                          | Default |
| ----------------- | ----------------------------- | ------- |
| `default_command` | `string` or `array of string` | `""`    |

The compile command for matching files without a database entry, without the source file: a string tokenized like a shell command line, or an argv array. It runs from the directory of the configuration file it was read from (the workspace root for a rule passed through initializationOptions), and the matching source files on disk join the background index (unless the rule turns `index` off) — enumerated at startup and again on every workspace poll, so a file created later compiles when opened and joins the index within a poll period. Omitted means none.

</div>

<div class="config-option">

| Option   | Type              | Default |
| -------- | ----------------- | ------- |
| `append` | `array of string` | `[]`    |

Compilation flags appended for matching files, e.g. `["-std=c++20", "-DNDEBUG"]`.

</div>

<div class="config-option">

| Option   | Type              | Default |
| -------- | ----------------- | ------- |
| `remove` | `array of string` | `[]`    |

Compilation flags removed for matching files, e.g. `["-Wall"]`.

</div>

<div class="config-option">

| Option  | Type   | Default |
| ------- | ------ | ------- |
| `index` | `bool` | `true`  |

Whether matching translation units join the background index. `false` keeps them out; they still compile when opened and still host the headers they include. Any matching rule saying `false` wins.

</div>

<div class="config-option">

| Option | Type   | Default |
| ------ | ------ | ------- |
| `lint` | `bool` | `true`  |

Whether `clice lint` checks matching files. `false` keeps them out: their findings are dropped and a translation unit they head is not parsed, unless `--index` needs it for the index. Files outside the workspace are never checked. Any matching rule saying `false` wins.

</div>

<div class="config-option">

| Option   | Type   | Default |
| -------- | ------ | ------- |
| `format` | `bool` | `true`  |

Whether `clice format` formats matching files. `false` keeps them out. Files outside the workspace are never formatted. Any matching rule saying `false` wins.

</div>

<!-- END GENERATED CONFIG -->

## Example

```toml
[[rules]]
compile_commands = ["build"]
append = ["-std=c++23"]

[[rules]]
patterns = ["test/**"]
append = ["-DTEST_MODE"]
```

Two build directories as switchable configurations, and a project without a compilation database:

```toml
default_configuration = "debug"

[[rules]]
configuration = "debug"
compile_commands = ["build/debug"]

[[rules]]
configuration = "release"
compile_commands = ["build/release"]
```

```toml
[[rules]]
patterns = ["src/**", "include/**"]
default_command = "arm-none-eabi-gcc -std=c23 -mcpu=cortex-m4 -Iinclude"
```

## Rules in Practice

Each example shows what a rule does to the files it matches.

<!-- BEGIN GENERATED ITEMS: project/rules -->

<!-- BEGIN CAPABILITY: supported -->

**Files without a database**

Files a rule matches compile with its default command, and files created later join the index

`src/main.cpp` compiles under the rule's command, with no note that the command was guessed. A file created while the server runs compiles the same way at once and is indexed at the next look at the disk.

```case
tests/project/rules/01_default_command
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Databases in priority order**

A file several databases list takes its command from the first one, and the others stay on offer as contexts

Database `a` lists `main.cpp` with `-DFROM_A`; database `b` lists it with `-DFROM_B` and also lists `other.cpp`. `main.cpp` compiles with the command from `a` and can be switched to the one from `b`, while `other.cpp` takes its command from `b`.

```case
tests/project/rules/03_database_order
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Patterns start at `clice.toml`**

A relative pattern matches paths under the directory of the `clice.toml` that declares it

The rule appends `-DFEATURE` to the files under `src/`: `src/main.cpp` compiles, while `other.cpp`, beside `clice.toml`, misses the macro.

```case
tests/project/rules/05_relative_patterns
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Remove flags from commands**

A rule takes flags out of a file's command, and a later rule's remove cancels an earlier append

The database gives `main.cpp` both `-DBROKEN` and `-include prefix.h`, which the first rule removes, so it compiles. For `other.cpp` the second rule appends `-DFEATURE` and the third removes it again.

```case
tests/project/rules/09_remove_flags
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Switching Configurations

The `configuration` tags on rules form a menu, and one tag is active per project; untagged rules always apply. The active one is, in priority order, the `--configuration <tag>` argument (accepted by `clice serve`, `clice index`, `clice lint` and `clice inspect`), the persisted selection, or `default_configuration`. A selection is made from the editor — in VS Code the status bar shows the active configuration and clicking it opens the menu, other clients call `clice/switchConfiguration` — and is stored in `state.json` under `cache_dir`, never in `clice.toml`; it takes effect when the server is started again, which the VS Code extension does on its own. Each configuration keeps its own index under `cache_dir`, so switching back and forth never reindexes what a configuration already indexed. The tags are also how the batch commands choose an index: `clice index --configuration release` builds the release index, and `clice index stats` reports the index of the configuration it resolves the same way.

<!-- BEGIN GENERATED ITEMS: project/configurations -->

<!-- BEGIN CAPABILITY: supported -->

**One command per board**

Tagged rules form a menu of build configurations, and a switch applies when the server starts again

Each board's rule gives its own default command, and `board-a` is the default configuration. Under it the header's `BOARD_B` branch is inactive; after switching to `board-b` and restarting, the `BOARD_A` branch is.

```case
tests/project/configurations/03_board_defaults
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
