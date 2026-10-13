# Class template members

A member of a class template shows the types the object's class is instantiated with

`Vector<Point>::at` returns `Point &`, not `T &`, and a call through a
function object shows the parameters of the function type it was declared
with. A member template's own parameters stay as it declares them. A standard
library type shows under its usual name: `string`, not `basic_string<char>`.
