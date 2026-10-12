---
option: rules.default_command
---

# Files without a database

Files a rule matches compile with its default command, and files created later join the index

`src/main.cpp` compiles under the rule's command, with no note that the command was guessed. A file created while the server runs compiles the same way at once and is indexed at the next look at the disk.
