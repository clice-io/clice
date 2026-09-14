# Lint

## Overview

clice integrates clang-tidy as a built-in linting engine. Standalone clang-tidy checks each translation unit on its own, so a header included by many sources is checked once per source. `clice lint` runs the same checks over the whole compilation database and checks each declaration once.

**Usage**: `clice lint [--workspace <dir>] [--configuration <tag>] [--workers <n>] [--index] [--no-dedup] [--verify]`

Runs clang-tidy over every translation unit in the compilation database with a
worker pool, prints the merged findings, and exits non-zero when problems are
found. `--index` additionally builds and persists the project index from the
same parses, so a follow-up `clice index` run has nothing left to do.

Exit codes: `0` for a clean run, `1` when there are findings, `2` when a
translation unit failed to run or the verification below failed.

## What is checked

- Every translation unit the compilation database lists, inside the workspace.
- Every header inside the workspace that the checked units include, subject to
  the `.clang-tidy` header filters (`HeaderFilterRegex`,
  `ExcludeHeaderFilterRegex`, `SystemHeaders`), read exactly as clang-tidy
  reads them.
- Files outside the workspace are never checked, whether or not the build marks
  them as system headers. A rule in `clice.toml` with `lint = false` keeps
  matching files inside the workspace out as well, for example a vendored
  library:

```toml
[[rules]]
patterns = ["third_party/**"]
lint = false
```

Each file takes its configuration from the nearest `.clang-tidy`, with the
inheritance clang-tidy applies. `NOLINT`, `NOLINTNEXTLINE` and
`NOLINTBEGIN`/`NOLINTEND` comments are honored in every file. Findings are
printed once each, sorted by file and position, with the notes clang-tidy
attaches to them.

## Cross-TU deduplication

clice hashes the content of every top-level declaration it parses: the
declaration's own text and the compile state it depends on (macros in effect,
diagnostic pragmas, the file's system status, the compile flags) together with
the declarations it refers to. Two translation units that see the same header
under the same flags produce the same hashes for its declarations, and a
declaration that one unit has already checked is skipped in the next.
Instantiations are tracked separately from their templates: a translation unit
that instantiates a template with new arguments re-checks the template for those
instantiations only.

A small set of checks looks at more than one declaration at a time (unused
using-declarations, include hygiene, naming conflicts across a file, and the
like); those run over every translation unit whole, and their findings are
merged like every other.

- `--no-dedup` checks every translation unit whole, as clang-tidy would.
- `--verify` does both and fails the run when the deduplicated findings differ
  from the whole runs. It costs a second parse per translation unit and exists
  to validate the deduplication on a code base, not for everyday use.

## clang-tidy Integration Quality

Issues that affect the quality of clang-tidy diagnostics within a language server:

- [ ] Suppress clang-tidy warnings from macros in system headers ([clangd#1587](https://github.com/clangd/clangd/issues/1587), [clangd#2000](https://github.com/clangd/clangd/issues/2000))
- [ ] Run checks on preprocessor directives in preamble (header guards, macros) ([clangd#2501](https://github.com/clangd/clangd/issues/2501), [clangd#160](https://github.com/clangd/clangd/issues/160))
- [ ] Configurable diagnostic severity per check category ([clangd#1937](https://github.com/clangd/clangd/issues/1937))
- [ ] Support loading clang-tidy plugins ([clangd#1458](https://github.com/clangd/clangd/issues/1458))
- [ ] Clang static analyzer support ([clangd#905](https://github.com/clangd/clangd/issues/905))
- [ ] Clean up replacements when applying clang-tidy fixes ([clangd#429](https://github.com/clangd/clangd/issues/429))
- [ ] Filter diagnostics by version control diff ([clangd#822](https://github.com/clangd/clangd/issues/822))
- [x] NOLINT / NOLINTNEXTLINE / NOLINTBEGIN-END comment suppression
- [ ] `Diagnostics.ClangTidy` configuration in `.clangd` config
- [ ] Fast-check filtering for clang-tidy performance
- [ ] Fix-it suggestions from clang-tidy as code actions
- [ ] Diagnostic metadata: check name, documentation URL, source tag
