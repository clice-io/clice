# Silencing with NOLINT

`NOLINT` comments silence findings on their line, and compiler warnings too

`// NOLINT` silences its own line and `// NOLINTNEXTLINE` the next, for every check or the ones named in parentheses. While clang-tidy runs on the file, they silence compiler warnings on those lines as well.
