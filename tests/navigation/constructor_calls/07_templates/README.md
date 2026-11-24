# Constructions in templates

A construction a template leaves open leads to the constructors it may call

Inside a template, `Box(T{}, T{})`, `Box<T>(value, value)` and
`Widget(size)` with a dependent argument call one of the class's
constructors taking that many arguments: their names lead to each of
them. `T(size)` names the template parameter, whatever an instantiation
constructs.
