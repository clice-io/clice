---
issues: [clangd#177]
---

# Reference enclosing context

Each reference reaches the editor as a bare location, without the function that encloses it

clangd attaches the enclosing function through an extension field
(`containerName`) for a client that declares support for it; clice does not
read that declaration yet. `clice query references` names the function each
reference lies in.
