---
issues: [clangd#2275]
---

# Types in templates

A variable in a template shows its type as the template would write it

A type that waits for an expression only instantiation can work out, such as `input + 1`, shows nothing, and neither does one the initializer already names. A template instantiated exactly once shows that instantiation's types instead.
