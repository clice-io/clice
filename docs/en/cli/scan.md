# Scan

## Overview

`clice scan` runs the dependency scan the server starts with: from every translation unit of the build it follows the includes, resolving each the way the compiler searches for it, and finds the module each unit provides. It prints what the scan found, without compiling anything and without touching the index.

**Usage**: `clice scan [--workspace <dir>] [--configuration <tag>] [--json | --runs <n>]`

The build is loaded as `clice index` loads it: the databases the rules name, else every `compile_commands.json` below the workspace. The summary counts the files the scan reached, the translation units among them and the includes it resolved, and lists every include no search found. `--json` prints every scanned file instead: whether it is a translation unit, the modules it provides, the headers it includes and the includes it could not resolve, as written.

`--runs <n>` times the scan: each run loads the build from scratch and scans it (cold, as at startup), then scans it again over what the first scan kept (warm, as after a database reload), and prints both times; the last line gives their minimum and median.

The exit code is 0 after a scan, 1 when no database lists a file under the workspace or the arguments ask for two outputs at once, 2 when they do not parse.

## Examples

### Includes

<!-- BEGIN GENERATED ITEMS: scan/includes -->

<!-- BEGIN CAPABILITY: supported -->

**Resolve includes**

Each file lists the headers its includes resolve to, and the includes no search finds

The scan resolves every `#include` the way the compiler searches for it: a quoted name from the includer's directory first, then the search path the command gives. It follows the headers it finds, and reports the includes it could not resolve by name.

```case
tests/scan/includes/01_resolution
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Modules

<!-- BEGIN GENERATED ITEMS: scan/modules -->

<!-- BEGIN CAPABILITY: supported -->

**Find module providers**

A module interface unit is listed with the module it provides

The scan reads each translation unit's module declaration, so the output names the file every module is built from.

```case
tests/scan/modules/01_providers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Several Commands

<!-- BEGIN GENERATED ITEMS: scan/commands -->

<!-- BEGIN CAPABILITY: supported -->

**One file, several commands**

A file the database compiles with several commands includes what each of them resolves

When the database lists one file under several commands, the scan resolves its includes under each command's search path, and the file lists every header they reach.

```case
tests/scan/commands/01_several_commands
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
