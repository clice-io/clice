---
issues: [clangd#445]
---

# Define all missing members

On the class name, every member function without a definition is defined at once, in declaration order

Members that already have a definition, pure virtuals and defaulted members are left alone.
