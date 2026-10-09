/// # Declaration-only specifiers dropped
///
/// - status: supported
///
/// `virtual`, `static`, `explicit`, `override` and `final` do not appear on the definition
///
/// Specifiers that must stay, such as `constexpr` and `noexcept`, are kept.

struct Base {
    virtual void draw() = 0;
};

struct Derived : Base {
    void §(draw)draw() override;
    static Derived §(make)make() noexcept;
    constexpr int §(size)size() const;
};
