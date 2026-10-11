---
issues: [clangd#445]
---

# Define in the host source

In a header, a member can also be defined in the source file the header is compiled with

The definition is fully qualified and joins the class's other definitions in that file; members already defined in some source file are not offered again. Templates, inline functions and functions other files cannot see stay in the header; any other function defined there out of the class is marked `inline`.
