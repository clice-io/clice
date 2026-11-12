// `virtual`, `static`, `explicit`, `override` and `final` stay off the
// definition, while `constexpr` and `noexcept`, which a definition must repeat,
// stay on.

struct Base {
    virtual void draw() = 0;
};

struct Derived : Base {
    void §(draw)draw() override;
    static Derived §(make)make() noexcept;
    constexpr int §(size)size() const;
};

struct Counter {
    unsigned static long §(interleaved)total();
    const static int §(const_static)limit();
};

template <class T>
struct Cell {
    explicit(sizeof(T) > 1) §(conditional_explicit)Cell(int value);
};
