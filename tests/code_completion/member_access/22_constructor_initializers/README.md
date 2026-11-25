# Constructor initializers

After a constructor's `:` the bases and members not initialized yet are offered, bases first, in declaration order

The one to initialize next comes first, though in a class with a virtual base it may not be. Each inserts the parameters of its constructor as placeholders, and a base or member with several constructors is one item, its parentheses left empty, unless overloads are listed apart. Names of other namespaces are not offered there.
