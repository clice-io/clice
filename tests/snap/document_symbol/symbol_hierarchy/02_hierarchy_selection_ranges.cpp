/// # Symbol ranges and selection ranges
///
/// - status: supported
///
/// The range spans the whole declaration; the selection range covers the full written name, including multi-token names like `~Widget`, `operator==` and `operator bool`

namespace members {

struct Widget {
    Widget();
    explicit Widget(int size);
    ~Widget();

    Widget& operator=(const Widget& other);
    bool operator==(const Widget& other) const;
    operator bool() const;

    static int instances();

    int size;
    unsigned bits : 3;
    const char* name = "widget";
};

Widget::Widget(int size) : size(size), bits(0) {}

int Widget::instances() {
    return 0;
}

}  // namespace members
