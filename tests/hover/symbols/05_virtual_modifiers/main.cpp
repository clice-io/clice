struct Base {
    virtual void dr§(pure_virtual)aw() = 0;
};

struct Circle : Base {
    void dr§(override_method)aw() override;
};

struct Dot final : Circle {
    void dr§(final_method)aw() final;
};

struct Square : Base {
    void dr§(implicit_override)aw();
};

struct Printable {
    virtual void draw() = 0;
};

struct Label : Base, Printable {
    void dr§(two_bases)aw() override;
};
