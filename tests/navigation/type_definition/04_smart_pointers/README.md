---
issues: [clangd#1026]
---

# Smart-pointer pointee navigation

Go-to-type-definition on a smart-pointer variable reaches both the wrapper
type and the type it points to
