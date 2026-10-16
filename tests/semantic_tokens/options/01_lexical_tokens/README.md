---
option: semantic_tokens.lexical
---

# Lexical tokens

With `lexical` off, only what the editor's own grammar cannot tell is sent

Keywords, literals, comments, built-in operators and directives are left
to the editor's grammar. Names, template brackets, declarator operators and
the code of untaken branches are still sent: an inactive token without a
kind of its own is `plain`, so the grammar's color stays and only dims.
