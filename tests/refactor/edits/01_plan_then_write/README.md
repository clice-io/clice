# Plan, then write

A dry run lists the edits without writing them, and a run without it rewrites the files

`--dry-run` answers with every name the rename would change, file by file, and
with the places it leaves alone for you to check: the body of `CALL` spells
`compute`, but a macro body names what each expansion makes of it. Without
`--dry-run` the files are rewritten. Renaming a class renames its constructors
with it.
