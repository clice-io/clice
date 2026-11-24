# Constructions in templates

A construction a template leaves open leads to the constructors it may call

Inside a template, `Box(T{}, T{})`, `Box<T>(value, value)`,
`Widget(size)`, `Widget{size, size}` and `new Widget(size)` with dependent
arguments call one of the class's constructors taking that many
arguments, a constructor template among them, any number for an expanded
pack: their names lead to each of them, and to what an instantiation
calls, a specialization's own constructor too. `T(size)` names the
template parameter, whatever an instantiation constructs.
