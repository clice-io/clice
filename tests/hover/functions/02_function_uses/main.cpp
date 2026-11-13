// A function's card at its uses: through a pointer, a call, a call before
// the definition, a call taking a default template argument, a call leaving
// default arguments out, a member defined out of line, a deprecated consteval
// function in nested anonymous namespaces and a static constexpr member.

namespace address_use {
void schedule(long) {}

auto* callback = &§(fn_via_pointer)schedule;
}

namespace call_use {
int calculate(int);

int run() {
    return §(fn_via_call)calculate(9);
}
}

namespace declaration_use {
void publish();
void invoke() {
    §(fn_decl)publish();
}
void publish() {}
}

namespace default_template_argument {
template <typename T = long> void consume(const T& = T()) {
    §(fn_default_tmpl_arg)consume<>(4L);
}
}

namespace defaults {

int compute(int a, int b = 10, int c = 20);

int result = comp§(call_site)ute(1);

}

namespace out_of_line {
struct Buffer {
    using size_type = unsigned long;
    size_type size() const;
};
Buffer::size_type Buffer::§(out_of_line_definition)size() const {
    return 0;
}
}

namespace specifiers {
namespace {
namespace {
[[deprecated("use a seed of your own")]] consteval int seed() { return 1; }
}
}

struct Factory {
    constexpr static Factory make(int left, double right) { return Factory(); }
};

int first = §(consteval_use)seed();
Factory made = Factory::§(static_member_use)make(1, 2.0);
}
