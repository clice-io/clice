struct Widget {
    §(constructor)Widget() {}
};

template <class T>
struct Holder {
    void reset() {
        auto copy = T();
    }
};

template struct Holder<Widget>;
