# X-macro lists

A list of macro calls compiles where its includer first expands it

`errors.def` is an X-macro list: `codes.cpp` includes it once to declare the enumerators and once to name them. Opened on its own, it compiles inside the enum, so its entries show as enumerators.
