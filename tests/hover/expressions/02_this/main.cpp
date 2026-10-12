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
