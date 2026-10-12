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

#define AUTO auto

void use() {
    au§(primary)to plain = make_box();
    au§(explicit_specialization)to chars = make_char_box();
    au§(partial_specialization)to pointers = make_pointer_box();
    au§(alias)to handle = make_handle();
    au§(builtin)to count = 1;
    au§(closure)to callback = [] {};
    Box<int>* allocated = new au§(new_expression)to(make_box());
    [§(init_capture)copy = make_box()] {};
    AU§(macro)TO spelled = make_box();
}
