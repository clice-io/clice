---
issues: [clangd#1634]
option: inlay_hints.block_end
---

# Names after closing braces

After the closing brace of a tall block, a hint names what the brace closes

Functions, types and control flow get one, when the block is at least `inlay_hints.min_block_lines` lines tall (ten by default; two here). `if` and `while` show their condition when it is short, an `else if` chain shows as plain `// if`, and a label longer than 60 characters is cut with `…`. Clicking a name goes to the start of its block.
