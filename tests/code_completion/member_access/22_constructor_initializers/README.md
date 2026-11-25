# Constructor initializers

After a constructor's `:` the bases and members not initialized yet are offered, bases first, in declaration order

The one to initialize next comes first, by the order the bases and members are declared in, even where C++ initializes a virtual base before the others. Each takes the arguments of its constructor, and a base or member with several constructors is one item, unless overloads are listed apart. Names of other namespaces are not offered there.
