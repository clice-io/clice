# Template subtypes

A class template deriving from a non-template base is one subtype of it

`template <typename T> struct Derived : Base` makes every specialization of
`Derived` derive from `Base`: the subtype is the template, `Derived`, not
one specialization such as the `Derived<Foo>` the code uses.
