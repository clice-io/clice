# Calls through an override

A call through a virtual method is an incoming call of every override it may dispatch to; a call through an override is not one of the method it overrides

`b.draw()` on a `Base&` may run `Base::draw` or `Derived::draw`, so both
list it among their incoming calls. `d.draw()` on a `Derived&` runs
`Derived::draw` or an override below it, never `Base::draw`, so only the
override lists it.
