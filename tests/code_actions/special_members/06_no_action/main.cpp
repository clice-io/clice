// The action has no body to append to on a forward declaration, nothing to
// declare on a union, and nothing to add to a class that declares all four
// operations itself, or its copies and destructor, which suppress the moves.

struct §(forward)Forward;

union §(value)Value {
    int integer;
    float real;
};

struct §(declared)Declared {
    Declared(const Declared&) = default;
    Declared(Declared&&) = default;
    Declared& operator=(const Declared&) = default;
    Declared& operator=(Declared&&) = default;
};

struct §(rule_of_three)RuleOfThree {
    RuleOfThree(const RuleOfThree&);
    RuleOfThree& operator=(const RuleOfThree&);
    ~RuleOfThree();
};
