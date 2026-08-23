/// # Special Hover Targets
///
/// ## `this` expression — hovering `this` shows the pointed-to class type
///
/// - status: supported
/// - order: 6
///
/// Works in a plain class and inside a class template.

namespace this_hover {

struct Widget {
    Widget* self() {
        return th§(plain_this)is;
    }
};

template <typename T>
struct Box {
    const Box* self() const {
        return th§(template_this)is;
    }
};

}
