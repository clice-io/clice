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
<!-- END GENERATED ITEMS -->

### Modules

<!-- BEGIN GENERATED ITEMS: scan/modules -->
<!-- END GENERATED ITEMS -->

### Several Commands

<!-- BEGIN GENERATED ITEMS: scan/commands -->
<!-- END GENERATED ITEMS -->
