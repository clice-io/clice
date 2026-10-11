# `new` expression

`new` navigates to an overloaded allocation function but not the constructor

Go-to-definition on `new` reaches the class's overloaded `operator new`.
The constructor invoked by the same expression is not part of the reply.
