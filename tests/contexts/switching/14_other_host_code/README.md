# Code other hosts compile

A header's code that only another host compiles keeps its colors, outline, folds, links and hover

`shared.h` defines `fast_path()` when `fast_host.cpp` includes it and `slow_path()` when `slow_host.cpp` does. Shown in the first host's context, the second branch is dimmed as inactive, yet it keeps its semantic colors, its outline entry, its folds and its include link, and hovering `slow_path` shows its declaration: clice's index holds every context of the header, and while the file is as it was saved, the answers take the other contexts' code from it. Hide other contexts, in the menu switching contexts, shows the first host's context alone.
