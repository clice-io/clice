#include "traits.h"

struct Variant {
    double §(get)get_d() const& {
        return 1;
    }

    double& get_d() & {
        return value;
    }

    double value = 0;
};

struct traits {
    struct get {
        CALL_TRAITS(d, get_d);
    };
};

double read(const Variant& variant) {
    return traits::get::d::member_function::call(variant);
}
