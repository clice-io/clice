// The enumerator summary of an enum: values as written, computed values for
// the rest.

namespace implicit_values {
enum Level { Low, Medium = 10, High };
§(implicit_values)Level level;
}

namespace written_values {
enum Permission : unsigned {
    Read = 1 << 0,
    Write = 1 << 1,
    Execute = 1 << 2,
    All = Read | Write | Execute,
};
§(written_values)Permission permission;
}

namespace scoped {
enum class Ordering : signed char { Less = -1, Equal, Greater };
§(scoped)Ordering ordering;
}

namespace empty {
enum class Tag : int {};
§(empty)Tag tag;
}

namespace dependent {
template <typename T> struct Holder {
    enum Size { None, Bytes = sizeof(T), Twice };
    §(dependent)Size size;
};
}
