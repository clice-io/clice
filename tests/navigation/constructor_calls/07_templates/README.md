# Constructions in templates

A construction a template leaves open leads to the constructors it may call

Inside a template, a construction its arguments or its type leave open —
`Box(T{}, T{})`, `Box<T>(value, value)`, `Box(value, value)` and, through an
alias template, `Crate(value, value)`, `Widget(size)`, `Widget{size, size}`,
`new Widget(size)`, `new Box<T>` — calls one of the class's constructors
taking that many arguments: a constructor template among them, any number
for a parameter pack or an expanded pack, a braced list of any length for
an `initializer_list` constructor, and one the class inherits, never a copy
or move constructor. Their names lead to each of them, and to what an
instantiation calls, a specialization's own constructor too. `T(size)`
names the template parameter, whatever an instantiation constructs, and
an aggregate's braces, `Pair<T>{}` under C++17, initialize its members: its
name stays the class's, while `Pair<T>()` calls its defaulted constructor.
