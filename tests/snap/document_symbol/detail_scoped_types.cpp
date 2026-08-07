/// # Symbol Detail
///
/// ## Scoped types — a written class scope survives in the detail exactly once, whether the type prints it itself (nested classes, aliases, dependent names) or the printer drops it and it is restored (template-ids)
///
/// - status: supported
/// - order: 6

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
