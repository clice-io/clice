/// # Using declarations
///
/// - status: supported
/// - issues: clangd#2619
///
/// The introduced name keeps its target's kind

namespace tools {
inline int helper() {
    return 1;
}
struct Gadget {};
}

using tools::§helper;
using tools::§Gadget;

int used = §helper();
§Gadget gadget;
