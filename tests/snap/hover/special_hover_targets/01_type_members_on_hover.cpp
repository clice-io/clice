/// # Members on type hover
///
/// - status: supported
/// - issues: clangd#959
/// - config: {"max_members": 2}
///
/// Hovering an enum or struct type lists its members
///
/// A class lists its data members and member types, an enum its
/// enumerators with their values; member functions are left out. The
/// `max_members` option caps the list, 20 entries by default, and `0`
/// turns it off.

namespace members {

enum Col§(enum_type)or {
    Red,
    Green = 4,
    Blue,
};

struct Poi§(struct_type)nt {
    using Scalar = double;
    Scalar x;
    Scalar y;
    Scalar length() const;
};

}
