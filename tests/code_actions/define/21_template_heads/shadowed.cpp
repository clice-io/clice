struct G {};

namespace app {
struct T {};

template <class T>
struct Shadow {
    ::app::T §(namespace_type)f();
    template <class G>
    ::G §(member_template_parameter)g();
};
}

struct T {};
struct Ω {};
template <char C>
struct Tag {};

template <class T, class Ω>
struct Global {
    ::T §(global_type)f();
    Tag<'T'> §(character_literal)tag();
    ::Ω §(non_ascii_name)omega();
};

struct C {};

template <class C>
struct Pointer {
    int (::C::*§(member_pointer_class)f())();
};
