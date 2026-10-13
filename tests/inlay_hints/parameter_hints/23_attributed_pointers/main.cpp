// An attribute on a function pointer variable rebuilds its type without the
// written parameters: the call names none.
using handler_t = void (*)(int code);
__attribute__((noreturn)) handler_t fail;

void use() {
    fail(1);
}
