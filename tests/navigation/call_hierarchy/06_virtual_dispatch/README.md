# Calls through an override

A call through a virtual method reaches every override it may dispatch to, a qualified call only the method it names

`b.draw()` on a `Base&` may run `Base::draw` or `Derived::draw`, so both
list it among their incoming calls. `d.draw()` on a `Derived&` runs
`Derived::draw` or an override below it, never `Base::draw`, so only the
override lists it. `Base::draw()`, in an override's body or anywhere else,
runs `Base::draw` alone, while `Group`'s `child->draw()` may reach either.
So does `Base::draw()` spelled by a macro, in `Logged`'s override.
