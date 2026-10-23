# Errors in headers' function bodies

An error inside a function body of a header shows once the background index has compiled the file as saved

The editor's compile of a file skips the bodies of the functions in the headers it includes at its top, as clangd does: the file opens and rebuilds after an edit several times faster. The background index compiles those bodies, and what it finds in them, an inline function's errors or a template's for the arguments the file instantiates it with, shows on the file while the file holds the text it compiled: an edit takes them down until the file is saved and compiled again. `skip_header_bodies = false` in `[project]` parses the bodies in the editor too.
