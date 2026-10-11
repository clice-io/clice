# Rewrite a program module

A directory of the program becomes a named module: its header a partition, its source an implementation unit, and the files including the header import the module

The header goes; the partition `app.core:text` takes its place, and the
primary interface `app/core/module.cppm` re-exports it.
