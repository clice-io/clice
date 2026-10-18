# Class body fragments

A file included inside a class body compiles as part of that class

`body.inc` holds members that `rec.cpp` includes between the braces of `Record`. Opened on its own, it compiles inside the class, so it has no errors, lists its members, and hover and go-to-definition work on them; completion offers the members the class declares after it too.
