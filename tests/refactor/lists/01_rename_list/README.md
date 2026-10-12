# Rename a list at once

A file listing renames, one `<symbol> => <new name>` a line, makes them all in one run

Every line is planned against the same index and the files are written once,
all of the renames or none of them. Blank lines and lines starting with `//`
are skipped.
