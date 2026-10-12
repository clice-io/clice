struct Widget {};

struct Pair {
    Widget first;
    int second;
};

Widget global;
Widget gallery[2];

void use() {
    au§(pointer_declarator)to* pointer = &global;
    const au§(const_reference)to& reference = global;
    au§(forwarding)to&& forwarded = global;
    au§(deduced_pointer)to address = &global;
    au§(array_reference)to& row = gallery;
    decltype(au§(decltype_auto)to) exact = global;
    au§(binding)to [first, second] = Pair{};
}
