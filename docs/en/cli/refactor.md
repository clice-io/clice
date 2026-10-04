# Refactor

## Overview

`clice refactor` rewrites the workspace's sources from the persisted index, the same index `clice query` reads. Nothing is compiled, so a refactoring over a large tree takes about as long as a query. `rename` is the one refactoring today; an editor's rename runs the same engine.

**Usage**: `clice refactor rename --to <name> (--name <query> | --symbol <id> | --path <file> --line <n>) [--workspace <dir>] [--configuration <tag>] [--dry-run] [--fresh]`

## Rename

The symbol is named as `clice query` names one: by a [name query](./query.md#name-queries), by the `#<hex>` id an earlier answer carried, or by the file and line defining it. A name several symbols spell lists them and asks for an id.

A rename changes every declaration, definition and reference the index ties to the symbol, and the symbols that must keep its name:

- a class's constructors and destructor — renaming a constructor renames its class;
- the explicit and partial specializations of a template, and the deduction guides of a class template;
- the overrides a virtual function is linked with, in base and derived classes.

Operators, conversion functions, user-defined literals, macros, symbols declared in system headers and symbols whose every declaration a macro spells cannot be renamed. The new name must be an identifier that is a keyword in neither C nor C++; a name reserved for the implementation (`__x`, `_X`, and `_x` at global scope) is a warning.

## Answers

The answer is one JSON object, `{"result": ..., "stale": [...]}`, like a query's:

- `files` lists the tokens replaced in each file, with a 1-based line and byte column. `heuristic` marks one reached through a name the index resolved by heuristics — a call in a template whose callee depends on a template parameter — worth a look.
- `unconfirmed` lists the places spelling the old name that the rename leaves alone because the index cannot say what they name: the body of a `#define`, a macro whose expansion spells the name, an inactive `#if` branch, a file no unit of the build compiles, a token that also names another symbol of the old name — under another build configuration, or as another overload a call in a template may pick. Read them before building.
- `warnings` are made anyway: the new name overloads a function of the same scope, hides or collides with a local of the same function, or is declared in a function using the renamed name, where it would capture that use.
- `conflicts` stop the rename: the new name is declared in the same scope already, is a macro, names a parameter or label of the same function, a member of a base or derived class, the class the renamed member belongs to or a member of the renamed class; the rename would edit a file outside the workspace; two namespaces would merge.
- `stale` lists the files the rename edits, or that spell the old or the new name, whose indexed rows are not current — changed since they were indexed, or never indexed while some unit of the build still is not. They stop the rename as well; `--fresh` reindexes them first.

The exit code is 0 when the rename was written, or planned under `--dry-run`, and 1 when a conflict or a stale file stopped it or the symbol cannot be renamed — with `{"error": "..."}` saying why in the last case. Arguments that do not parse exit with 2.

## Writing

Without `--dry-run` the command writes the files once every new text is computed: a file whose text changed since the plan was made stops the whole rename before anything is written. Each file is rewritten in place, keeping its byte order mark, line endings and permissions; a file that cannot be written — read-only, or a full disk — stops the rename there, and the error names the files already rewritten.

Only the workspace is edited: hidden directories, the cache directory and build trees (a directory holding `CMakeCache.txt` or `build.ninja`) are left out. Its sources and headers are searched for the two names by suffix; an edit the index places may land in a file of any suffix.

The command reads the disk; unsaved editor buffers are invisible to it. Save them first, or rename from the editor.

## In the editor

The editor's rename selects the name under the cursor, or says why the symbol there cannot be renamed. The edits it returns are versioned against the open buffers, which it reads instead of the disk. Conflicts and stale files answer an error naming them; warnings, the places left alone and the edits made through heuristics are shown in a message.

## Known Limitations

- The checks are static: a change of meaning the index cannot see — argument-dependent lookup finding another function, a member of the new name capturing a use inside its class, an overload that cannot coexist with the renamed function — goes unnoticed. Build after renaming.
- Macros cannot be renamed yet.
- An editor's rename edits through the index of the project serving the file; another project open in the same editor keeps its uses of the symbol.
