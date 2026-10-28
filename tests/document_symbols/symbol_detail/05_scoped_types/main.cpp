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

void take(Outer::Box<int> boxed, Outer::Inner plain);

template <typename T>
struct Holder {
    typename T::type value;
    typename T::inner::type deep;
    typename T::template rebind<int> bound;
};

}  // namespace scoped
