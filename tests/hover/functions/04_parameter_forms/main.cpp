// Parameter cards for the forms clang prints apart from how they are written:
// a parameter of function type, explicit object parameters, a definition that
// inherits its default argument from the declaration before it.

namespace function_type {
void retry(int §(function_type)attempt(int));
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
