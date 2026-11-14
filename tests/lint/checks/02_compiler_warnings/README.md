# Compiler warnings as findings

A warning the build enables is a finding unless the checks turn it off

The compiler warnings a unit's command enables are findings, reported under their `clang-diagnostic-<flag>` name, as clang-tidy reports them. A `Checks` starting with `-*`, as here, turns them off but for those it names.
