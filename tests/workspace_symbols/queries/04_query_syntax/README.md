# Query syntax

Quotes, wildcards, scopes and filters narrow a search

`"process"` matches the whole name only and `proc*` whatever it globs;
`io::*` lists a namespace's members and `io::**` its whole subtree;
`kind:function` keeps one kind. Terms combine, separated by spaces.
VS Code's symbol search sends only the first term of a query and filters the
answers by it: there, terms after the first, and a filter alone, find
nothing.
