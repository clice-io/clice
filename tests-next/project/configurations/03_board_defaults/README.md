---
option: rules.configuration
---

# One command per board

Tagged rules form a menu of build configurations, and a switch applies when the server starts again

Each board's rule gives its own default command, and `board-a` is the default configuration. Under it the header's `BOARD_B` branch is inactive; after switching to `board-b` and restarting, the `BOARD_A` branch is.
