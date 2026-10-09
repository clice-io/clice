namespace imported_function {
namespace library {
void open();
}
void invoke() {
    using library::open;
    §(01_using_function)open();
}
}

namespace imported_overloads {
namespace library {
void write(long);
void write(char);
}
using library::write;
template <typename T> void invoke() {
    §(02_using_overloads)write(T{});
}
}

namespace declaration_site {
namespace library {
void close(long);
}
using library::§(03_using_decl_site)close;
}

namespace imported_type {
struct Base {
    using index_type = long;
};
struct Derived : Base {
    using Base::index_type;
    §(04_using_type)index_type index;
};
}

namespace namespace_alias {
namespace implementation {}
namespace api = implementation;
using namespace §(05_alias_directive)api;
}
