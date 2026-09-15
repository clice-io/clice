# Format

## Overview

`clice format` runs clang-format over the files of a workspace, several
processes at a time, with the `.clang-format` configuration the tree already
has. clice adds no style settings of its own: the style comes from
clang-format's own discovery, the nearest `.clang-format` above each file.

**Usage**: `clice format [--workspace <dir>] [--configuration <tag>] [--check] [--clang-format <path>] [--jobs <n>] [<path>...]`

Without paths it formats the build's own files: every translation unit of the
compilation database and every file with a C-family suffix they include that
sits inside the workspace, outside the build trees (a directory below the root
holding a compilation database, where a build generates or fetches sources),
outside the system include directories of their commands and outside the
cache directory. A
directory argument narrows that set to the
files under it; a file argument with a C-family suffix is formatted whether or
not the build knows it. Paths are relative to the workspace. A rule in `clice.toml` with
`format = false` keeps matching files out, for example a vendored library or
generated sources:

```toml
[[rules]]
patterns = ["third_party/**", "gen/**"]
format = false
```

Files are rewritten in place. `--check` rewrites nothing and prints what
clang-format would change, one diagnostic per place, then how many files need
formatting. `--clang-format` names the executable to run, by default the
`clang-format` found in `PATH`.

Exit codes: `0` when every file is formatted (or, with `--check`, nothing
would change), `1` when `--check` found files to format, `2` when the run
could not complete: clang-format was not found, it failed on a file, or the
requested configuration does not exist.
