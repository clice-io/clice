struct Widget {
    int value;
    Widget& operator=(const Widget& other);
};

void take(§(auto)auto item) {}

void forms(Widget a, Widget b) {
    Widget* made = ::§(new)new Widget;
    int §(member_class)Widget::*field = &Widget::value;
    §(assigned)a = b;
    ::delete made;
}

template <class T>
int call(T object) {
    return object.template §(dependent_template)get<int>();
}
