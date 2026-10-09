/// # Symbol kind
///
/// - status: supported
///
/// The card names what the symbol is: struct, enum, function, field, …

namespace ki§(namespace)nds {

struct Poi§(struct)nt {
    int §(field)x;
};

union Pack§(union)et {
    int raw;
};

enum class Col§(enum)or {
    R§(enumerator)ed,
};

using Ali§(typedef)as = Point;

int leng§(function)th(Point p) {
    return p.x;
}

}
