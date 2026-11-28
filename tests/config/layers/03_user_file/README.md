# The user's file

Settings for every project go in the user's configuration file, under each project's own

`clice/config.toml` in the user's configuration directory (`~/.config` on Linux and macOS, `%APPDATA%` on Windows, `$XDG_CONFIG_HOME` wherever it is set) applies to every project, under its `clice.toml`. Here the user's rule and the project's both give `main.cpp` a macro, and the file compiles; `clice inspect config` lists the files read and where each setting comes from.
