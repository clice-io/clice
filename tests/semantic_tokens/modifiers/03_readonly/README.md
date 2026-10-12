# Readonly

Const values and methods, plus enum members, carry the readonly modifier

Readonly is currently value-based: a pointer to const counts as
readonly even though the pointer itself can change.
