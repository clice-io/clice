# Library-internal aliases

An `auto` deduced through a library's internal alias reaches the type the alias names; one deduced through a public alias reaches the alias

A name a library reserves for itself, `std::__detail::__unique_ptr_t`, is
not one the code would write, so `auto` sees through it to `unique_ptr`. A
public alias such as `iterator` is the name the library gives its users.
