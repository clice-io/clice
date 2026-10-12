# `#include_next`

`#include_next` and `__has_include_next` link to the next header of that name on the search path

Here `first/wrap.h` shadows `second/wrap.h`, and its `#include_next` reaches
the second copy.
