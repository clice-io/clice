# Query syntax

Quotes, wildcards, scopes and filters narrow a search

`"process"` matches the whole name only and `proc*` whatever it globs;
`io::*` lists a namespace's members and `io::**` its whole subtree;
`kind:function` keeps one kind. Terms combine, separated by spaces.
