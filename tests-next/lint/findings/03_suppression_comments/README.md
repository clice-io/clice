# Suppression comments

NOLINT comments silence findings in headers as well as in the file being checked

`// NOLINT` silences its own line, `// NOLINTNEXTLINE` the next one, and `// NOLINTBEGIN` and `// NOLINTEND` everything between them, for every check or for the ones named in parentheses. They count in every file a unit reads, as clang-tidy reads them.
