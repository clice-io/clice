struct Widget {
    ~Widget();
    static void operator delete(void* memory);
};

void dispose(Widget* widget) {
    §(delete)delete widget;
}
