namespace evaluated_call {
constexpr long multiply(long left, long right) {
    return left * right;
}
long §(01_constexpr_call_value)product = multiply(3, 4);
}

namespace evaluated_size {
long §(02_sizeof_value)bytes = sizeof(double);
}

namespace evaluated_template_member {
template <long Left, long Right> struct Product {
    static constexpr long value = Left * Right;
};
long §(03_template_member_value)result = Product<3, 5>::value;
}

namespace evaluated_enumerator {
enum class Status { Idle = -4, Ready = 12 };
Status current = Status::§(04_enumerator_value)Ready;
}

namespace symbolic_enum_value {
enum class Status { Idle = -4, Ready = 12 };
Status source = Status::Idle;
Status copy = §(05_enum_var_value)source;
}

namespace evaluated_static_member {
template <long Base> struct Offset {
    static constexpr long value = Base + 6;
};
long result = Offset<10>::val§(06_static_member_value)ue;
}

namespace aliased_result {
using count_type = long;
constexpr count_type total() {
    return 18;
}
long result = to§(07_constexpr_fn_value)tal();
}

namespace pointer_value {
const char* §(08_string_pointer_value)message = "ready";
}

namespace dependent_constructor {
template <typename T> struct Holder {
    Holder(long identifier);
};
template <typename T> void create(long identifier) {
    new Holder<T>(iden§(09_dependent_ctor_arg)tifier);
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
    product::api::detail::Resp§(10_skip_inline_anon_ns)onse response;
}

namespace incomplete_template_argument {
template <typename T> struct Wrapper {};
class Forward;
void create() {
    §(11_auto_incomplete_arg)auto wrapper = Wrapper<Forward>{};
}
}
