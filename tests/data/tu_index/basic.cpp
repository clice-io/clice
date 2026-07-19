// Seed case for the per-feature snapshot corpus: symbols and relations.
namespace demo {

struct Base {
    virtual ~Base() = default;
    virtual int value() const;
};

struct Derived : Base {
    int value() const override;
};

int use(const Base& b) {
    return b.value();
}

}  // namespace demo
