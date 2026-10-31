#include "filler.h"
#include "lib.h"

namespace app {

int run() {
    return helper§(using_declaration);
}

}  // namespace app

namespace lib {
helper§(first_in_namespace)
}  // namespace lib

using namespace lib;

int use() {
    return helper§(using_directive);
}

int qualified() {
    return lib::helper§(namespace_member);
}

int all_members() {
    return lib::§(namespace_names);
}

int nested_qualified() {
    return lib::detail::helper§(two_qualifiers);
}

int absolute() {
    return ::lib::helper§(absolute);
}

int global() {
    return ::helper§(global);
}

int through_declaration() {
    return app::helper§(qualified_using_declaration);
}

int through_directive() {
    return nested::helper§(qualified_using_directive);
}

auto made() -> helper§(trailing_return);

int local_directive() {
    using namespace lib::detail;
    return helper§(local_using_directive);
}

int local_alias() {
    namespace d = lib::detail;
    return d::helper§(local_alias);
}

int command_line() {
    return HELPER_FROM§(command_line_macro);
}

long compiler() {
    return __cplusp§(compiler_macro);
}

int aliased() {
    return inner::§(namespace_alias);
}
