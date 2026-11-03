# Types read left to right

A type C++ writes inside out reads left to right: `*`, `&` and `[N]` follow what they apply to, a function reads `(params) -> R`, a member pointer `C::-> T`

A function or member pointer inside another is named by a Greek letter, spelled on the line below.
