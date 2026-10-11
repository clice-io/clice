---
issues: [clangd#1026]
---

# Smart-pointer pointee navigation

Go-to-type-definition on a smart-pointer variable reaches the wrapper type
itself; unwrapping to the pointee type is not offered
