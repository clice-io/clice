struct Widget {
    ~Widget();
};

void dispose(Widget* widget) {
    §(delete)delete widget;
}
