/// # Type-level layout
///
/// - status: partial
/// - issues: clangd#1763
///
/// Hovering the type itself shows its size, alignment and padding
///
/// Size and alignment show on the type card today; the total padding
/// does not yet.

namespace layout {

struct Wid§(struct_size)get {
    int id;
    double value;
};

}
