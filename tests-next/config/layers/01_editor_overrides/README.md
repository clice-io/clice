# Editor settings over the file

Rules the editor sends replace the rules of `clice.toml`

`clice.toml` gives `main.cpp` the macro it uses, and the file compiles. An editor that sends its own `rules` in its settings replaces the file's rules rather than adding to them, so here the macro is gone.
