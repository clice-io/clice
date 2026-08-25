/// # Special Hover Targets
///
/// ## Members on type hover — hovering an enum or struct type lists its members
///
/// - status: partial
/// - order: 1
/// - issues: clangd#959
///
/// The card names the type (and a struct's layout), but the member list is
/// not expanded — the body renders as `{}`.

namespace members {

enum Col§(enum_type)or {
    Red,
    Green,
    Blue,
};

struct Poi§(struct_type)nt {
    int x;
    int y;
};

}
