# Unnameable types stay auto

Lambdas, dependent types, structured bindings and types the declaration cannot name are not expanded

A type cannot be named where it is local to another function, a member type the declaration has no access to, or the type of `sizeof` with no standard name for it declared yet (MSVC compatibility declares `size_t` implicitly).
