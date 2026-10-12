struct Guard {
    explicit operator bool() const;
};

void use(Guard g) {
    if §(condition)(g) {}
    for §(for_condition)(; g;) {}
    do {} §(do_condition)while (g);
    bool ok = §(negation)!g;
    bool both = g §(conjunction)&& ok;
    bool either = ok §(disjunction)|| g;
    int pick = g §(conditional)? 1 : 0;
}
