// Names brought in by using-declarations show what they name: a function,
// overloads where they are brought in and in a template, a member type,
// inherited constructors; a namespace alias in a using-directive.

namespace imported_function {
namespace library {
void open();
}
void invoke() {
    using library::open;
    §(using_function)open();
}
}

namespace imported_overloads {
namespace library {
void write(long);
void write(char);
}
using library::§(using_overloads_site)write;
template <typename T> void invoke() {
    §(using_overloads)write(T{});
}
}

namespace declaration_site {
namespace library {
void close(long);
}
using library::§(using_decl_site)close;
}

namespace imported_type {
struct Base {
    using index_type = long;
};
struct Derived : Base {
    using Base::index_type;
    §(using_type)index_type index;
};
}

namespace namespace_alias {
namespace implementation {}
namespace api = implementation;
using namespace §(alias_directive)api;
}

namespace inherited_overloads {
struct Base {
    Base(int value);
    Base(double value);
};
struct Derived : Base {
    using Base::§(inherited_constructors)Base;
};
}
