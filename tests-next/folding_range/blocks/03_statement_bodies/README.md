# Statement bodies

The bodies of control-flow statements and bare blocks fold

Each branch of an `if`/`else` or a `try`/`catch` folds on its own: the line
that closes one branch and opens the next stays visible.
