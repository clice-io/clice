struct Iterator {
    int operator*() const;
    Iterator& operator++();
    bool operator!=(const Iterator& other) const;
};

struct Range {
    Iterator begin();
    Iterator end();
};

struct Widget {
    Widget(const Widget& other);
};

void use(Range r, Widget w) {
    for(int x§(after_variable): r) {
    }
    Widget copy§(after_copy)=w;
}
