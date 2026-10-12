struct Guard {
    explicit operator bool() const;
};

void use(Guard g) {
    if §(condition)(g) {}
    bool ok = §(negation)!g;
}
