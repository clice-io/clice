---
option: inlay_hints.block_end
---

# Names after closing braces

After the closing brace of a block spanning lines, a hint names what the brace closes

Functions, types, namespaces and control flow get one; `if` and `while` show their condition when it is short, an `else if` chain shows as plain `// if`, and a label longer than 60 characters is dropped. A block on one line gets none.
