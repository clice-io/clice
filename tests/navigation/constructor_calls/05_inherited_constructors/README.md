# Inherited constructors

A class's name calling a constructor it inherits leads to the base's

`Derived(1)`, where `Derived` inherits its constructors with
`using Base::Base`, runs the constructor `Base` declares: the name leads
there, and go-to-type-definition to `Derived`, the class it constructs.
