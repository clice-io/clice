/// # `auto` deduced as templates and aliases
///
/// - status: supported
/// - verify: server
///
/// An `auto` deduced as a template specialization reaches the template, or
/// the explicit or partial specialization it selects; one deduced through an
/// alias reaches the alias
///
/// A builtin type or a lambda's closure type has no declaration to reach.

template <typename T>
struct Box {};

template <>
struct Box<char> {};

template <typename T>
struct Box<T*> {};

using Handle = Box<long>;

Box<int> make_box();
Box<char> make_char_box();
Box<int*> make_pointer_box();
Handle make_handle();

void use() {
    au§(primary)to plain = make_box();
    au§(explicit_specialization)to chars = make_char_box();
    au§(partial_specialization)to pointers = make_pointer_box();
    au§(alias)to handle = make_handle();
    au§(builtin)to count = 1;
    au§(closure)to callback = [] {};
}
