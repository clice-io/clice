---
issues: [clangd#493, clangd#1768]
---

# Generic lambda parameters

An `auto` parameter of a lambda completes the members of the type the lambda is called with

The call can come after the lambda, or be the algorithm the lambda is
passed to: completion reads how the code around it instantiates the
lambda. A lambda called with several types completes nothing.
