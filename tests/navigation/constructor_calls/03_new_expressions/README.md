# `new` expressions

The class name a `new` allocates leads to the constructor it runs

In `new Widget(1)`, `new Widget{2}` and `new Widget`, the class's name
answers the constructor that builds the object, and go-to-type-definition
the class. An array's element type names the class alone.
