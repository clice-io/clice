// An attribute on a function pointer variable rebuilds its type without the
// written parameters: the call names none, unlike one through the plain
// alias.
using handler_t = void (*)(int code);
__attribute__((noreturn)) handler_t fail;
handler_t plain;

void use() {
    plain(1);
    fail(2);
}
