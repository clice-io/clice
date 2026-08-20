/// # Designator Hints
///
/// ## Aggregates only — constructor calls, copies and idiomatic zero-init produce no designators
///
/// - status: supported
/// - order: 5

struct Constructible {
    Constructible(int amount);
};

// A braced constructor call names parameters, not fields.
Constructible built{5};

struct Copyable {
    int x;
};

Copyable original{1};
Copyable duplicate{original};

// The idiomatic `{}` zero-initializer stays quiet.
struct Wide {
    int fields[8];
};

Wide zeroed{};
