struct Guard {
    explicit operator bool() const;
};

void use(Guard g) {
    if §(condition)(g) {}
    bool ok = §(negation)!g;
    bool both = g §(conjunction)&& ok;
    int pick = g §(conditional)? 1 : 0;
}
