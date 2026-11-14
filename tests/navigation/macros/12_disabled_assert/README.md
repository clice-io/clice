# Disabled assertions

The names inside a disabled `assert` go nowhere

Under `NDEBUG`, `assert(...)` expands to nothing and its argument is never
compiled: the names it spells have no definition to go to and are not among
the references. clice does not guess what they would name.
