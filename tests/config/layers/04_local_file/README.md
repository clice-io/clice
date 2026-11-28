# The local file

`.clice/clice.local.toml` holds a developer's own settings for a project whose `clice.toml` is committed

The project's `clice.toml` compiles `main.cpp` as C++17 with a macro; the local file, which the cache directory's `.gitignore` keeps out of Git, asks for C++20 and takes the macro away. Its rules come before the project's, so they decide first, and their flags come after, so they win on the command line: the concept compiles and the `#error` stays out.
