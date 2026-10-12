// After a dot or an arrow, the destructor, the class's own name, its
// bases' names and its operators are left out: a member access almost
// never names them. After `::` they are all offered.
struct Base {
    int base_field;
};

struct Widget : Base {
    int width;
    ~Widget();
    int operator[](int index) const;
    explicit operator bool() const;
    void resize(int width);
};

void use(Widget widget, Widget* pointer) {
    widget.§(dot);
    pointer->§(arrow);
    Widget::§(scope);
}
