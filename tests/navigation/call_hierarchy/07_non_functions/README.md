---
issues: [clangd#1308]
---

# Non-function targets

Call hierarchy preparation returns nothing for variables and enum constants

Preparing a call hierarchy on a variable or an enum constant returns
nothing; the request is offered only for functions and methods.
