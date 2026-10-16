---
option: semantic_tokens.lexical
---

# Lexical tokens

With `lexical` off, only what the editor's own grammar cannot tell is sent

Keywords, literals, comments, built-in operators and directives are left
to the editor's grammar. Names, template brackets and declarator operators
are still sent, and so is the code of untaken branches, as `plain` tokens:
the grammar's color stays and only dims.
