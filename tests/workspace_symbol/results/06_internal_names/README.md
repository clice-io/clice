# Names of internal linkage

Search finds the names of internal linkage at namespace scope, in every file declaring one, but never a function's locals

A `static` variable or a member of an anonymous namespace is known only to
its own file, yet it is a name the project declares: search lists it, once
per file that declares one. A local variable is not.
