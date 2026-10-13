---
issues: [clangd#1807]
option: inlay_hints.min_block_lines
---

# Block heights

Only a block ten lines or taller gets a hint at its end

The height counts the lines of both braces. `inlay_hints.min_block_lines` sets it for block ends, namespace ends and `#endif` alike.
