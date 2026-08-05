---
name: build
description: Build clice. Optional arg = build type (Debug | RelWithDebInfo, default RelWithDebInfo). Runs in a forked context — compile output and mechanical fixes stay out of the main conversation; only the outcome returns.
context: fork
---

Build the project with the requested build type (default `RelWithDebInfo`).

- Full build (configure + build): `pixi run build [type]`
- Configure only: `pixi run cmake-config [type]`
- Build only (skip configure): `pixi run cmake-build [type]`
- Specific target: `cmake --build build/[type] --target [target]` (common targets: `clice`, `unit_tests`)

On failure:

- Mechanical breakage (missing include, renamed symbol, stale call site after an agreed-on change): fix it, rebuild, and list every file you touched in the report.
- Design-level errors (the fix requires a decision): do not guess — report the error with `file:line` and the relevant excerpt.

Report back: build type, success or failure, files changed (if any), and for failures a digested error list — never the raw compiler spew.
