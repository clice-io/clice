# clice

C++ language support powered by [clice](https://github.com/clice-io/clice), a
language server written from scratch on LLVM/Clang: template-aware completion
inside generic code, first-class compilation contexts for headers and build
configurations, and native C++20 modules support.

## Getting started

1. Install this extension — the clice server for your platform is bundled, no
   download or extra setup needed. Cursor, VSCodium, Windsurf and other VS Code
   forks install it from [Open VSX](https://open-vsx.org/extension/clice-io/clice).
2. Open a C++ project with a
   [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html).
   For CMake: `cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`. clice
   searches the workspace root and its immediate subdirectories (e.g.
   `build/`) automatically.

See the [documentation](https://docs.clice.io/clice/) for configuration via
`clice.toml` and the full feature guide.

## Release channels

Stable versions use even minor numbers; daily pre-releases built from `main`
use odd ones. Use `Switch to Pre-Release Version` on the extension page to
follow the pre-release channel. Per-commit builds and standalone server
downloads live on the [GitHub releases and CI pages](https://github.com/clice-io/clice#install).

## Settings

| Setting                         | Default     | Description                                                                                                                                                                        |
| ------------------------------- | ----------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `clice.executable`              | _(bundled)_ | Path to a clice binary to use instead of the bundled one.                                                                                                                          |
| `clice.mode`                    | `pipe`      | Server transport; `socket` connects to an external server (debugging).                                                                                                             |
| `clice.host`                    | `127.0.0.1` | Host for socket mode.                                                                                                                                                              |
| `clice.port`                    | `50051`     | Port for socket mode.                                                                                                                                                              |
| `clice.serverEnv`               | `{}`        | Environment of the server and the compilers it asks, on top of VS Code's; `${env:NAME}` names VS Code's variables. A Cygwin or MSYS2 compiler needs its runtime's `bin` on `PATH`. |
| `clice.trace.server`            | `off`       | Log LSP traffic to the `clice (LSP trace)` output channel.                                                                                                                         |
| `clice.initializationOptions`   | `{}`        | Configuration in the shape of `clice.toml`, taking the place of the same settings in the project's file.                                                                           |
| `clice.inactiveRegions.opacity` | `0.45`      | Opacity of preprocessor branches not taken; `1` leaves them as they are.                                                                                                           |

Changing a server setting offers to restart the server in place — no window
reload is needed; saving `clice.toml` or `.clice/config.toml` restarts it on
its own, as clice asks (not when it connects to a server over a socket). With
[Even Better TOML](https://marketplace.visualstudio.com/items?itemName=tamasfe.even-better-toml)
installed, both configuration files are completed and checked against clice's
schema.

## What clice tells of a file

The language status of a C or C++ file — the `{}` beside its language mode;
pin an item to keep it in the status bar — tells:

- where its compile command comes from: `database` (an entry of a
  compilation database), `host` (the command of a source including the
  header), `default` (a rule's `default_command`), `inferred` (borrowed from
  a unit nearby) or `fallback` (clice's own), marked as a warning when the
  command is clice's own or its compiler could not be queried. **Show**, or
  `clice: Show Compile Command`, opens the whole command;
- its compilation context, when it has more than one: `auto` or `selected`,
  with **Switch**. The Explorer's Compilation Contexts view lists them all;
- the build configuration, when the rules in `clice.toml` declare any, with
  **Switch**, which restarts the server under the one picked.

A compile that runs long shows its progress in the status bar: the file and
what it waits on (its modules, its preamble, the parse). What needs fixing in
the setup, such as a workspace without a compilation database, shows as a
notification.

## Refactorings

clice's refactorings are behind the lightbulb, `Ctrl+.` and **Refactor...**
in the editor's context menu. Each also has a command in the Command Palette
("clice: Define Out of Line", "clice: Add Missing Enum Cases to Switch", ...)
that a key can be bound to; the
[code action guide](https://docs.clice.io/clice/features/code-action#running-actions)
lists them.

## Conflicting extensions

Other C/C++ language extensions running next to clice duplicate completion
and diagnostics. Once clice is up, it detects the Microsoft C/C++
(cpptools), clangd and ccls extensions and offers to turn their language
features off and reload the window — cpptools keeps its debugger, only
IntelliSense is disabled.

## Troubleshooting

Server logs live in the `clice` output channel, which also prints the
on-disk log directory at startup (`Session log directory:` — by default
under `<workspace>/.clice/logs/`). If the server crashes,
please attach the newest log from there to a
[GitHub issue](https://github.com/clice-io/clice/issues) — releases ship
symbol packages that let us reconstruct the exact stack.

For protocol-level debugging, set `clice.trace.server` to `verbose`: every
LSP message then appears in the `clice (LSP trace)` output channel.

Extension development is documented in the
[contributor guide](https://docs.clice.io/clice/dev/extension).
