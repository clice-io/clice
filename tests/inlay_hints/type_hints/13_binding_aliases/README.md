# Binding aliases

The bindings of a container's elements keep the aliases the container was declared with

Iterating a `std::map<std::string, Point>` shows each key as a
`const std::string`, where the standard library's own instantiation names
only `basic_string<char>`. Two aliases of one type leave it unaliased.
