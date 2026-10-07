// - verify: both
//
// A conversion function is named after the type as written, not its
// canonical form: an alias stays an alias, a template parameter keeps its
// name.

namespace lib {
struct basic_string {};
using string = basic_string;
}  // namespace lib

using Handle = int*;

template <class T>
struct Box {
    operator T&();
    operator Handle();
    operator lib::string();
    explicit operator bool();
};
