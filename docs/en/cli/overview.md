# Command Line Overview

clice is one binary. Editors launch `clice serve` and talk to it over LSP; the other subcommands drive the same engine from a terminal, sharing its compilation database handling, caches and index.

| Command      | What it does                                                                          | Page                           |
| ------------ | ------------------------------------------------------------------------------------- | ------------------------------ |
| `serve`      | Start the language server. Editors do this for you, see the editor setup.             | [editors](../guide/editors.md) |
| `lint`       | Run clang-tidy over every translation unit with a worker pool.                        | [lint](./lint.md)              |
| `index`      | Index a workspace ahead of time so the server starts warm.                            | [index](./index.md)            |
| `format`     | Format the workspace's files with clang-format.                                       | [format](./format.md)          |
| `inspect`    | Run one feature on a source file and print the raw result as JSON.                    |                                |
| `command`    | Show the commands a file compiles with, and convert compile commands for other tools. | [command](./command.md)        |
| `query`      | Ask the persisted index about symbols, references, call graphs and files.             | [query](./query.md)            |
| `refactor`   | Rename a symbol across the workspace from the persisted index.                        | [refactor](./refactor.md)      |
| `analyze`    | Report facts for refactoring, such as module dependencies and cycles.                 | [analyze](./analyze.md)        |
| `modularize` | Wrap third-party libraries as C++20 modules, rewrite the program into module units.   | [modularize](./modularize.md)  |
| `scan`       | Scan the include and module dependencies of the build, and time the scan.             | [scan](./scan.md)              |

`inspect` works today but has no page yet; its operations and options are documented by `clice inspect --help`.

## Arguments

Every subcommand takes its arguments the same way:

```
clice <command> [<operation>] [<target>...] [--option...]
```

- A command that does several things names the one to do by a word after it: `clice query references`, `clice index stats`, `clice analyze modules edge`. A command whose own job comes first runs it when no word follows: `clice index` indexes, `clice analyze modules` gives the overview, `clice command <file>` shows the file's commands. `--help` after any command or operation lists what it takes.
- The target, the symbol or file an operation is about, follows the operation: `clice query references "shapes::Circle::area"`, `clice query document-symbols src/main.cpp`. A symbol is named by a [locator](./query.md#locators) wherever one is taken; a file by its path, relative to the workspace (for `inspect`, to the current directory) or absolute. The files a command reads or writes itself, such as a partition or an output directory, are relative to the current directory.
- Options come after the operation. An option takes several values by being repeated: `--scope 'src/**' --scope 'include/**'`.
- `--workspace <dir>` names the workspace, the current directory by default (`inspect` takes the project the file is in, as the editor does); `--configuration <tag>` pins the build configuration over the selection persisted from the editor (see [switching configurations](../guide/configuration.md#switching-configurations)); `-j`, `--jobs <n>` is how many units or files `index`, `lint` and `format` work on at once; `--log-level` sets what is logged to stderr, warnings and errors by default (`serve`, whose stderr an editor shows, from `info`); the session logs of `serve`, `index` and `lint` keep `info` whatever it is.
