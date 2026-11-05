# Dependent expression members

A member accessed on what a dependent subscript, call or `auto` variable evaluates to resolves to the member declared on the class template

Where the call has a `const` overload, the constness of the object picks
the one it names; among overloads that differ in their parameters, one
taking the arguments' types exactly is the one named, and explicit
template arguments name only the templates.
