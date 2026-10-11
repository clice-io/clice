# Choosing a header's host

A header that relies on its includer reads the way the chosen includer makes it

`shared.h` uses a macro that each source defines before including it. Both sources are contexts of the header: clice picks one, the editor can switch to the other, and hover then shows the type that host gives. The choice is kept across restarts until it is reset.
