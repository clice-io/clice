---
outline: 2
---

# Configuration

clice reads configuration from `clice.toml` in the workspace root, or from `.clice/config.toml` if the former does not exist. Configuration can also be passed via LSP `initializationOptions` (JSON format); values from `initializationOptions` override the config file, and defaults fill in whatever remains unset after the merge.

Configuration is read once at server startup. Changing it — either file — requires restarting the server; there is no hot reload.

A JSON schema of the whole configuration is published at [`clice-config.schema.json`](/clice-config.schema.json); editors that validate TOML or JSON against a schema can point at it.

Relative paths and patterns in the file resolve against the directory of the configuration file itself; values passed through `initializationOptions` resolve against the workspace root.

## Variable Substitution

The following variable is supported in string values:

| Variable       | Description                                    |
| -------------- | ---------------------------------------------- |
| `${workspace}` | The workspace directory provided by the client |

## Workspace

Top-level options, written before any section.

<!-- BEGIN GENERATED CONFIG: root -->

<div class="config-option">

| Option             | Type              | Default |
| ------------------ | ----------------- | ------- |
| `compile_commands` | `array of string` | `[]`    |

Compilation databases for the whole workspace, in priority order: a compile_commands.json or a directory containing one, relative to this configuration file. Equivalent to a trailing rule without patterns; see `[rules].compile_commands`. When neither this nor any rule declares a source, the workspace root and its immediate subdirectories are searched for one.

</div>

<div class="config-option">

| Option                  | Type     | Default |
| ----------------------- | -------- | ------- |
| `default_configuration` | `string` | `""`    |

The build configuration active at startup, one of the tags declared on rules. When rules carry tags and this names none of them, the first declared tag is used and a warning is logged.

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

Initial number of stateless workers — they handle ephemeral tasks (PCH/PCM builds, completion, signature help); defaults to half the machine's parallelism, at least 2. `0` is invalid and falls back to that default.

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

Upper bound for dynamic stateless-worker scaling; `0` means the machine's parallelism, which is also the default.

</div>

<!-- END GENERATED CONFIG -->

## `[tracker]`

The file tracker polls for changes that happen outside the editor (a `git checkout`, a regenerated `compile_commands.json`, code generators writing headers) so the server picks them up without a restart. Setting an interval to `0` disables that polling loop.

<!-- BEGIN GENERATED CONFIG: tracker -->

<div class="config-option">

| Option             | Type     | Default |
| ------------------ | -------- | ------- |
| `cdb_poll_seconds` | `uint32` | `3`     |

Compilation database poll interval in seconds; 0 disables polling.

</div>

<div class="config-option">

| Option                   | Type     | Default |
| ------------------------ | -------- | ------- |
| `workspace_poll_seconds` | `uint32` | `30`    |

Workspace file sweep interval in seconds; 0 disables polling.

</div>

<!-- END GENERATED CONFIG -->

## `[hover]`

The `[hover]` section controls how hover cards render.

<!-- BEGIN GENERATED CONFIG: hover -->

<div class="config-option">

| Option                      | Type   | Default |
| --------------------------- | ------ | ------- |
| `parse_comment_as_markdown` | `bool` | `true`  |

Render the hover card as markdown; `false` produces plain text for clients that cannot display it.

</div>

<div class="config-option">

| Option     | Type   | Default |
| ---------- | ------ | ------- |
| `show_aka` | `bool` | `true`  |

Show the desugared form of a type, e.g. `vector<int>::size_type (aka unsigned long)`.

</div>

<!-- END GENERATED CONFIG -->

## `[inlay_hints]`

The `[inlay_hints]` section controls which inlay hint categories the server produces. A client-side refresh then requests hints with the updated values; no recompile is involved.

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

Parameter name hints at call sites, e.g. `draw(width: 800, height: 600)`, including `&` markers for arguments passed by mutable reference.

</div>

<div class="config-option">

| Option          | Type   | Default |
| --------------- | ------ | ------- |
| `deduced_types` | `bool` | `true`  |

Deduced type hints for `auto` variables, structured bindings and deduced return types.

</div>

<div class="config-option">

| Option        | Type   | Default |
| ------------- | ------ | ------- |
| `designators` | `bool` | `true`  |

Field designator hints in aggregate initialization, e.g. `.x=` and `.y=` in `Point{1, 2}`.

</div>

<div class="config-option">

| Option      | Type   | Default |
| ----------- | ------ | ------- |
| `block_end` | `bool` | `false` |

`// name` hints after the closing brace of long blocks (functions, types, namespaces, control flow).

</div>

<div class="config-option">

| Option              | Type   | Default |
| ------------------- | ------ | ------- |
| `default_arguments` | `bool` | `false` |

Show the default arguments a call omitted, abbreviated when long.

</div>

<div class="config-option">

| Option            | Type     | Default |
| ----------------- | -------- | ------- |
| `type_name_limit` | `uint32` | `32`    |

Byte budget for rendered hint text: over-long deduced types fall back to a sugared spelling or are dropped, over-long default arguments are abbreviated. `0` means no limit.

</div>

<!-- END GENERATED CONFIG -->

## `[code_completion]`

The `[code_completion]` section controls completion item assembly.

<!-- BEGIN GENERATED CONFIG: code_completion -->

<div class="config-option">

| Option                   | Type   | Default |
| ------------------------ | ------ | ------- |
| `enable_keyword_snippet` | `bool` | `false` |

Complete keywords as snippets (not yet implemented).

</div>

<div class="config-option">

| Option                              | Type   | Default |
| ----------------------------------- | ------ | ------- |
| `enable_function_arguments_snippet` | `bool` | `false` |

Insert function arguments as a snippet when completing a call. For functions this applies to individually listed overloads, so it requires `bundle_overloads = false`; function-like macros have no overload sets and always take the snippet.

</div>

<div class="config-option">

| Option                              | Type   | Default |
| ----------------------------------- | ------ | ------- |
| `enable_template_arguments_snippet` | `bool` | `false` |

Insert template arguments as a snippet on completion (not yet implemented).

</div>

<div class="config-option">

| Option                          | Type   | Default |
| ------------------------------- | ------ | ------- |
| `insert_paren_in_function_call` | `bool` | `false` |

Insert parentheses when completing a function call (not yet implemented).

</div>

<div class="config-option">

| Option             | Type   | Default |
| ------------------ | ------ | ------- |
| `bundle_overloads` | `bool` | `true`  |

Collapse an overload set into a single completion item.

</div>

<div class="config-option">

| Option  | Type     | Default |
| ------- | -------- | ------- |
| `limit` | `uint32` | `0`     |

Maximum number of completion items (not yet implemented).

</div>

<!-- END GENERATED CONFIG -->

## `[[rules]]`

A rule names files by pattern and says where they take their compile commands from and how those commands are edited. Every rule matching a file applies, in declaration order: an earlier rule's databases rank first among the file's candidates, the first matching rule with a `default_command` supplies the command of a file without an entry, `append` and `remove` accumulate with a later `remove` cancelling an earlier `append`, and `index = false` on any matching rule keeps the file out of the index. A rule carrying a `configuration` tag applies only while that configuration is active; the distinct tags form the configuration menu.

<!-- BEGIN GENERATED CONFIG: rules -->

<div class="config-option">

| Option     | Type              | Default |
| ---------- | ----------------- | ------- |
| `patterns` | `array of string` | `[]`    |

Glob patterns selecting the files this rule applies to. A relative pattern is anchored at this configuration file's directory (`..` segments allowed); an absolute pattern or one starting with `**` matches the file's absolute path. `*` matches within a path segment, `?` a single character, `**` any number of segments, `{a,b}` alternatives, `[0-9]` a character range, `[!...]` a negated range. Omitted means every file.

</div>

<div class="config-option">

| Option          | Type     | Default |
| --------------- | -------- | ------- |
| `configuration` | `string` | `""`    |

Build configuration tag. A tagged rule applies only while that configuration is active; an untagged rule always applies. The distinct tags form the configuration menu, and `default_configuration` names the one active at startup.

</div>

<div class="config-option">

| Option             | Type              | Default |
| ------------------ | ----------------- | ------- |
| `compile_commands` | `array of string` | `[]`    |

Compilation databases, in priority order: a compile_commands.json or a directory containing one, relative to this configuration file. All of them load, and every entry applies to its own file whatever the patterns say; the patterns and the order decide which entry a file present in several databases gets by default.

</div>

<div class="config-option">

| Option            | Type                          | Default |
| ----------------- | ----------------------------- | ------- |
| `default_command` | `string` or `array of string` | `""`    |

The compile command for matching files without a database entry, without the source file: a string tokenized like a shell command line, or an argv array. It runs from this configuration file's directory, and the matching source files on disk join the background index — enumerated at startup, so a file created later compiles when opened and joins the index at the next start. Omitted means none.

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

<!-- END GENERATED CONFIG -->

## Example

```toml
compile_commands = ["build"]

[[rules]]
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
