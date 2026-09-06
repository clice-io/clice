/// # Typedef underlying struct
///
/// - status: partial
/// - issues: clangd#2020
///
/// Hovering an alias expands the aliased definition
///
/// The card resolves the alias to its underlying type name, but does not
/// expand that struct's definition or member list.

namespace aliases {

struct Widget {
    int id;
    double value;
};

using Han§(alias_using)dle = Widget;

typedef Widget Wid§(alias_typedef)get_t;

}
