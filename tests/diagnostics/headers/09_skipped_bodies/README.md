# Errors in headers' function bodies

An error inside a function body of a header shows once the background index has compiled the file as saved

The editor's compile of a file skips the bodies of the functions in the headers it includes at its top, as clangd does: the file opens and rebuilds after an edit several times faster. The background index compiles those bodies, and what it finds in them, an inline function's errors, a template's for the arguments the file instantiates it with, or those of the file's own generic lambda such a template calls, shows on the file while the file holds the text it compiled: an edit takes them down until the file is saved and compiled again. A rule setting `skip_function_body = false` for a header parses its bodies in the editor too; one setting `skip_function_body = true`, for third-party code, skips them in the background index as well, and what goes wrong in them never shows.
