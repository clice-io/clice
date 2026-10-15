// Parameter cards for the forms clang prints apart from how they are written:
// parameters of function type, written as one or through an alias, explicit
// object parameters, a parameter with an attribute, a definition that
// inherits its default argument from the declaration before it.

namespace function_type {
void retry(int §(function_type)attempt(int));
void retry_quietly(int §(noexcept_function_type)attempt(int) noexcept);
void install(void (*§(returns_function_pointer)handler(int))(double));
using Callback = void(int);
void subscribe(Callback §(alias_function_type)callback);
}

namespace explicit_object {
struct Widget {
    void resize(this Widget& §(explicit_object)self, int width);
    template <typename Self> auto& get(this Self&& §(deduced_object)self);
};
}

namespace inherited_default {
void open(int mode = 0644);
void open(int §(inherited_default)mode) {}
}

namespace attributed {
void report([[maybe_unused]] int §(attributed)code);
}
