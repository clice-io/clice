# Compilation

clice compiles each open file with its command from the compilation database, as the build would. The sections below cover what that takes on real projects: the flags a command passes, languages beside C and C++, file encodings, files that change on disk, and the cache that outlives a restart.

<!-- The cards in the sections below are generated from the documented cases
     in tests/compile/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## Compile Flags

Flags of a file's command that change what it compiles, such as `-include`, apply in the editor as in the build.

<!-- BEGIN GENERATED ITEMS: compile/flags -->

<!-- BEGIN CAPABILITY: supported -->

**Forced includes**

Headers a command forces in with `-include` work like the ones a file includes itself

Build systems force headers in for configuration macros and precompiled headers. Their macros hover and navigate, and editing them reaches every file they are forced into.

```case
tests/compile/flags/01_forced_include
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Precompiled headers**

The units of a target with precompiled headers share one PCH, built by clice from the build's command

CMake gives every unit of such a target the precompiled header it builds from `cmake_pch.hxx.cxx`. The file on disk may come from another compiler, so clice builds its own from the same command: the target's headers are parsed once for every file open and for the index, and editing one of them rebuilds the PCH and reaches every unit.

```case
tests/compile/flags/07_precompiled_headers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Other Languages

Files of other languages a build compiles, such as assembler sources, are left alone: clice analyzes only C-family code.

<!-- BEGIN GENERATED ITEMS: compile/languages -->

<!-- BEGIN CAPABILITY: supported -->

**Assembler sources**

Assembler sources a build compiles, preprocessed or not, are left to the assembler: the index, lint and the editor skip them

```case
tests/compile/languages/01_assembler
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## File Encodings

A file works the same whatever its byte order mark or line endings.

<!-- BEGIN GENERATED ITEMS: compile/encoding -->

<!-- BEGIN CAPABILITY: supported -->

**Byte order marks**

Files saved as UTF-8 with a byte order mark work like any other

Positions on the first line skip the mark, as the editor shows it, and data a file embeds keeps its bytes.

```case
tests/compile/encoding/01_byte_order_mark
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Windows line endings**

Files whose lines end in CRLF answer at the same places as any other

```case
tests/compile/encoding/02_line_endings
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Files Changing on Disk

A header another tool rewrites reaches the files that include it, without being opened.

<!-- BEGIN GENERATED ITEMS: compile/staleness -->

<!-- BEGIN CAPABILITY: supported -->

**Changes outside the editor**

A header changed on disk reaches the files that include it at their next request, saved or not

Checkouts, generators and other tools rewrite headers behind the editor's back. The next request about an including file compiles it against the new text, wherever the header sits in its includes, while a rewrite of the same bytes costs nothing.

```case
tests/compile/staleness/01_header_edits
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Cache

What clice compiles for headers is kept on disk, so a restart starts warm.

<!-- BEGIN GENERATED ITEMS: compile/cache -->

<!-- BEGIN CAPABILITY: supported -->

**Warm restarts**

Precompiled headers outlive a restart, and changes made while the server was down still reach the files that use them

```case
tests/compile/cache/01_headers_restart
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
