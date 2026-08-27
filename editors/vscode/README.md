# clice

C++ language support powered by [clice](https://github.com/clice-io/clice), a
language server written from scratch on LLVM/Clang: template-aware completion
inside generic code, first-class compilation contexts for headers and build
configurations, and native C++20 modules support.

## Getting started

1. Install this extension — the clice server for your platform is bundled, no
   download or extra setup needed.
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

| Setting              | Default     | Description                                                            |
| -------------------- | ----------- | ---------------------------------------------------------------------- |
| `clice.executable`   | _(bundled)_ | Path to a clice binary to use instead of the bundled one.              |
| `clice.mode`         | `pipe`      | Server transport; `socket` connects to an external server (debugging). |
| `clice.host`         | `127.0.0.1` | Host for socket mode.                                                  |
| `clice.port`         | `50051`     | Port for socket mode.                                                  |
| `clice.trace.server` | `off`       | Log LSP traffic to the `clice (LSP trace)` output channel.             |

Changing a server setting offers to restart the server in place — no window
reload is needed.

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
