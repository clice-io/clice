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
