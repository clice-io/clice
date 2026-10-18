# Format

## Overview

`clice format` formats the files of a workspace with the clang-format built
into clice, as the editor does, several files at a time, with the
`.clang-format` configuration the tree already has. clice adds no style
settings of its own: the style comes from clang-format's own discovery, the
nearest `.clang-format` above each file.

**Usage**: `clice format [<path>...] [--workspace <dir>] [--configuration <tag>] [--check] [--jobs <n>]`

Without paths it formats the build's own files: every translation unit of the
compilation database and every file with a C-family suffix they include that
sits inside the workspace, outside the build trees (a directory below the root
holding a compilation database, where a build generates or fetches sources,
units included), outside the system include directories of their commands and
outside the cache directory. A
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

Files are rewritten in place; a symlinked source is rewritten at its target,
which has to sit inside the workspace as well. `--check` rewrites nothing and
prints what clang-format would change, one diagnostic per place, then how many
files need formatting.

Exit codes: `0` when every file is formatted (or, with `--check`, nothing
would change), `1` when `--check` found files to format, `2` when the run
could not complete: the arguments do not parse, a file or its `.clang-format`
could not be read, the workspace, its configuration or a compilation
database could not be found or loaded, or the requested configuration does
not exist.

## Examples

### The Build's Files

Without paths, the build's own files are formatted; named paths narrow the run or add files to it.

<!-- BEGIN GENERATED ITEMS: format/files -->

<!-- BEGIN CAPABILITY: supported -->

**Format the build's files**

Without paths, the build's sources and the headers they include are formatted; vendored and system headers are left alone

```case
tests/format/files/01_build_files
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Format named paths**

A named file is formatted whether or not the build knows it; a named directory narrows the build's files to those under it

```case
tests/format/files/02_named_paths
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Rewriting in Place

A run rewrites every file that does not follow its style yet.

<!-- BEGIN GENERATED ITEMS: format/rewrite -->

<!-- BEGIN CAPABILITY: supported -->

**Format in place**

Without `--check` every file of the build is rewritten to its style

```case
tests/format/rewrite/01_format_in_place
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Checking

`--check` reports what would change and writes nothing.

<!-- BEGIN GENERATED ITEMS: format/check -->

<!-- BEGIN CAPABILITY: supported -->

**Check formatting**

With `--check` nothing is rewritten; each place clang-format would change is reported

```case
tests/format/check/01_check_formatting
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
