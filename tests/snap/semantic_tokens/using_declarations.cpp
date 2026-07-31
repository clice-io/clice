/// # Declarations & References
///
/// ## Using declarations — the introduced name keeps its target's kind
///
/// - status: supported
/// - issues: clangd#2619
/// - order: 10

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
