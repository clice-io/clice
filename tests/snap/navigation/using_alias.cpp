/// - verify: server

namespace core {

int §(target_def)compute(int value) {
    return value;
}

}

namespace §(ns_alias)api = §(ns_alias_target)core;

using core::§(using_decl)compute;

int run(int value) {
    return §(direct_use)compute(value) + api::§(qualified_use)compute(value);
}
