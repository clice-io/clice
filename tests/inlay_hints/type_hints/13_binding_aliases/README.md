# Binding aliases

The bindings of a container's elements keep the aliases the container was declared with

Iterating a `std::map<std::string, Point>` shows each key as a
`const std::string`, not as `basic_string<char>`. Two aliases of one type,
or an alias and the type itself, leave it unaliased.
