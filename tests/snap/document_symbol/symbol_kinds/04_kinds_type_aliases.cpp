/// # Type aliases
///
/// - status: supported
///
/// `typedef`, `using` aliases and alias templates appear in the outline with a
/// `type alias` detail

namespace aliases {

struct Widget {};

typedef Widget LegacyWidget;

using ModernWidget = Widget;

template <typename T>
struct Box {};

template <typename T>
using BoxOf = Box<T>;

struct Holder {
    using Inner = Widget;
};

}  // namespace aliases
