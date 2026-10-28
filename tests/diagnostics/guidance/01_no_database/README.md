# No compilation database

A file no compilation database lists compiles with a guessed command and says so

Without a `compile_commands.json` entry, a file still compiles, with a command clice guesses. A workspace without any database is told so in a message the editor shows, and when the guess leaves an include unresolved, a warning at the top of the file explains why and links to the setup guide. Once the database lists the file, the warning goes, and the errors left are the code's own.
