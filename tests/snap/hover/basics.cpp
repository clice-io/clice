/// Describes a free function.
void ren§(01_global_func)der() {}

namespace studio::detail {
/// Describes a namespaced function.
void flu§(02_ns_func)sh() {}

class Canvas {
    char la§(03_field)yer;
    double scale[2];
};

}

union Pixel {
    char ch§(04_union_field)annel;
    double opacity;
};

struct Permissions {
    unsigned int rea§(05_bitfield)d : 1;
};

struct PackedPermissions {
    char prefix;
    unsigned int wri§(06_bitfield_padding)te : 1;
    int suffix;
};

namespace studio::detail {
struct Painter {
    void paint() {
        int bru§(07_method_local)shes = 0;
    }
};
}

namespace studio {
namespace {
struct {
    char mo§(08_anon_struct_field)de;
} state;
}
}

struct §(09_struct_size)EmptyCanvas {};

void current_function() {
    __f§(10_predefined_var)unc__;
}

template <int N> void dependent_function() {
    __f§(11_predefined_var_dependent)unc__;
}
