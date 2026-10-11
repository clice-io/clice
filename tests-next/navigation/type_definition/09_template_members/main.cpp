// A member of a class template has the type its specialization gives it,
// through an alias template too, and a variable template's specialization
// has its own: in Box<Widget> value is a Widget, in Boxed<Gadget> a Gadget,
// and zero<Widget> is a Widget.

struct Widget {
    int size;
};

struct Gadget {};

template <typename T>
struct Box {
    T value;
    T get() const;
};

template <typename T>
using Boxed = Box<T>;

template <typename T>
T zero = T{};

int read(Box<Widget>& boxed, Boxed<Gadget>& aliased) {
    auto copy = boxed.§(method)get();
    Gadget gadget = aliased.§(alias_member)value;
    return boxed.§(member)value.size + copy.size + §(variable_template)zero<Widget>.size;
}
