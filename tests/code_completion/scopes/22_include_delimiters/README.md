---
option: code_completion.angled_includes
---

# Include delimiters

An inserted `#include` takes quotes or angle brackets by the directory that finds the header, unless the configuration says otherwise

A header next to the file or found through `-iquote` or `-I` takes quotes; one found through `-isystem` or `-idirafter` takes angle brackets. `angled_includes` and `quoted_includes` choose them by the header's path instead, and a header both match takes quotes. The standard library's headers always take angle brackets.
