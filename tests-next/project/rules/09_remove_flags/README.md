---
option: rules.remove
---

# Remove flags from commands

A rule takes flags out of a file's command, and a later rule's remove cancels an earlier append

The database gives `main.cpp` both `-DBROKEN` and `-include prefix.h`, which the first rule removes, so it compiles. For `other.cpp` the second rule appends `-DFEATURE` and the third removes it again.
