// The action has no body to append to on a forward declaration, no name to
// spell the members with on an unnamed class, and nothing to add to a class
// that declares all four operations itself, or its copies and destructor,
// which suppress the moves.

struct §(forward)Forward;

struct §(unnamed){
    int x;
} unnamed;

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
