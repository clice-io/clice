# Cast conversion navigation

A cast navigates to the constructor or conversion operator it runs

Go-to-definition on a `static_cast`, on the parenthesis of a C-style cast
or of a functional cast written with a class's name, or on the builtin type
of a functional cast (`double(m)`), reaches the constructor it selects or
the user-defined conversion operator it calls.
