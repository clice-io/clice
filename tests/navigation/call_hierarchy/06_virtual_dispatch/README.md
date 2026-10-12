# Calls through an override

A call through a class that overrides a virtual method is an incoming call
of the override alone

`d.draw()` on a `Derived&` runs `Derived::draw` or an override below it,
never `Base::draw`, so only the override lists it among its incoming calls.
