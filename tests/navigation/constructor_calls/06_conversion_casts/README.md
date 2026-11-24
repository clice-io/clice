# Conversion casts

A functional cast through a conversion function names the type

`Target(source)`, when `Source` converts to `Target` with
`operator Target()`, writes the type `Target`, which is the name of the
conversion function too: the name leads to the class, and the
parenthesis, as a C-style cast's does, to the conversion function.
