# Compiler warnings as findings

A warning the build enables is a finding only when the checks name it

The compiler warnings a unit's command enables are findings when `Checks` lists their `clang-diagnostic-<flag>` name, reported under that name. The others stay out of the report.
