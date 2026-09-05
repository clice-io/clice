namespace address_use {
void schedule(long) {}

auto* callback = &§(01_fn_via_pointer)schedule;
}

namespace call_use {
int calculate(int);

int run() {
    return §(02_fn_via_call)calculate(9);
}
}

namespace declaration_use {
void publish();
void invoke() {
    §(03_fn_decl)publish();
}
void publish() {}
}

namespace default_template_argument {
template <typename T = long> void consume(const T& = T()) {
    §(04_fn_default_tmpl_arg)consume<>(4L);
}
}
