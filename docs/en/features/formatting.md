# Formatting

## Formatting a Document

Formatting a file (`textDocument/formatting`) lays it out in its clang-format style and sorts its includes in the same pass.

<!-- BEGIN GENERATED ITEMS: format/document -->

<!-- BEGIN CAPABILITY: supported -->

**Format a document**

Formatting a file lays all of it out in the style its `.clang-format` asks for

```case
tests-next/format/document/01_format_document
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Includes sorted**

Formatting also sorts each block of includes, as the style asks

```case
tests-next/format/document/04_include_sorting
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Formatting a Selection

Range formatting (`textDocument/rangeFormatting`) reformats only the lines a selection touches.

<!-- BEGIN GENERATED ITEMS: format/range -->

<!-- BEGIN CAPABILITY: supported -->

**Format a selection**

Formatting a selection reformats only the lines it touches

```case
tests-next/format/range/01_format_range
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Style Resolution

Each file takes the style of the `.clang-format` nearest above it, as clang-format finds it.

<!-- BEGIN GENERATED ITEMS: format/style -->

<!-- BEGIN CAPABILITY: supported -->

**Nearest style file**

Each file takes the style of the `.clang-format` nearest above it

```case
tests-next/format/style/01_nearest_style
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

- [x] Fallback to LLVM default style when no `.clang-format` is found in any parent directory

## On-type and Save Hooks

- [ ] On-type formatting (`textDocument/onTypeFormatting`)
- [ ] Format-on-save integration

## Project-wide Format

Beyond the LSP `textDocument/formatting` request (which formats a single open file), clice provides project-wide formatting via CLI: [`clice format`](../cli/format.md) formats every file of the build, several at a time, and `--check` reports what it would change.

- [ ] Incremental format (only modified files since last run)
