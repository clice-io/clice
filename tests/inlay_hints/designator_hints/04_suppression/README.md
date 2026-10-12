# Designator suppression

Written designators and `/*name=*/` comments keep their inits bare

Mixing written designators with positional initializers is a C99 extension clang accepts with a warning; only the bare `4` needs a hint.
