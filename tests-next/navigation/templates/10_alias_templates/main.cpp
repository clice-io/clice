// An alias template is a name of its own, and a member reached through it
// is the class template's.

template <typename T>
struct Box {
    T value;
};

template <typename T>
using Boxed = Box<T>;

int read(§(alias_use)Boxed<int> boxed) {
    return boxed.§(member)value;
}
