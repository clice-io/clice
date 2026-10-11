# Directory modules

Each directory is a module, and a file naming another module's entity makes the dependency

A header naming another module's entity is an interface edge, which would
become an import of the module's interface; a source alone makes an
implementation edge. Outside a git repository the churn annotation is left
out, with a warning.
