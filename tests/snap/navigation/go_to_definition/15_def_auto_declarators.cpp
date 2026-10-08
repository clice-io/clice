/// # `auto` behind pointers and references
///
/// - status: supported
/// - issues: clangd#2055
/// - verify: server
///
/// Pointers, references and arrays around the deduced type are stripped:
/// `auto*`, `const auto&`, `auto&&`, `decltype(auto)` and an `auto` deduced
/// as a pointer all reach the class
///
/// The `auto` of a structured binding reaches the type of the object it
/// decomposes.

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
