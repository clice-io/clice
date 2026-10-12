// Values past the documented case: an enum variable, a static member of an
// instantiation, a constexpr function's call, a string pointer, a dependent
// constructor argument, a class behind inline and anonymous namespaces, auto
// of an incomplete template argument.

namespace symbolic_enum_value {
enum class Status { Idle = -4, Ready = 12 };
Status source = Status::Idle;
Status copy = §(enum_var_value)source;
}

namespace evaluated_static_member {
template <long Base> struct Offset {
    static constexpr long value = Base + 6;
};
long result = Offset<10>::val§(static_member_value)ue;
}

namespace aliased_result {
using count_type = long;
constexpr count_type total() {
    return 18;
}
long result = to§(constexpr_fn_value)tal();
}

namespace pointer_value {
const char* §(string_pointer_value)message = "ready";
}

namespace dependent_constructor {
template <typename T> struct Holder {
    Holder(long identifier);
};
template <typename T> void create(long identifier) {
    new Holder<T>(iden§(dependent_ctor_arg)tifier);
}
}

namespace product {
inline namespace current {
namespace api {
namespace {
namespace detail {
inline namespace latest {
class Response {};
}
}
}
}
}
}
void receive() {
    product::api::detail::Resp§(unwritten_namespaces)onse response;
}

namespace incomplete_template_argument {
template <typename T> struct Wrapper {};
class Forward;
void create() {
    §(auto_incomplete_arg)auto wrapper = Wrapper<Forward>{};
}
}
