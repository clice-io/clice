# Errors in headers' function bodies

An error inside a function body of a header shows on the file that instantiates it, as the file is edited

The editor's compile parses the function bodies of the headers a file includes, and instantiates the templates the file uses: an inline function's errors, a template's for the arguments the file instantiates it with, or those of the file's own generic lambda such a template calls, show on the file as it is edited. The parse's own diagnostics come first: the file answers its requests while clang still instantiates, and what the instantiations raise follows.
