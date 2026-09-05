/// # Scoped types
///
/// - status: supported
///
/// A written class scope appears in the detail exactly once, for nested classes, template-ids, aliases and dependent names alike

namespace scoped {

struct Outer {
    struct Inner {};
    template <typename T> struct Box {};
    using Alias = int;
};

struct User {
    Outer::Inner plain;
    Outer::Box<int> boxed;
    Outer::Alias aliased;
    const Outer::Inner frozen;
};

template <typename T>
struct Holder {
    typename T::type value;
    typename T::inner::type deep;
    typename T::template rebind<int> bound;
};

}  // namespace scoped
