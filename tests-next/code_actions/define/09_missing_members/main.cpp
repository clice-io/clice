struct §(all)Widget {
    Widget();
    ~Widget() = default;
    virtual int size() const = 0;
    void reset();
    void grow(int by);
    int id() const { return 0; }
};

void Widget::reset() {
}
