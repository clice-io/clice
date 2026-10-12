---
issues: [clangd#493, clangd#1768]
---

# Generic lambda parameters

A generic lambda's `auto` parameter is the type the lambda is called with, and the parameter's members have cards

Inside the body the parameter reads as the type of the call, and `point.x`
is the member of that type. A lambda called with several types keeps its
parameter `auto`.
