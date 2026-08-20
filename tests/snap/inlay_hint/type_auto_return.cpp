/// # Type Hints
///
/// ## Deduced return types — `-> T` after the parameter list, declarations included
///
/// - status: supported
/// - order: 5

auto answer() {
    return 42;
}

auto& ref_answer() {
    static int storage = 0;
    return storage;
}

// A declaration hints once a later definition supplies the deduction; a
// definition-less one stays silent.
auto declared(int x);
auto deducible(int x);

auto deducible(int x) {
    return x + 1;
}

// Written trailing return types need no hint.
auto spelled() -> int;
auto pointer() -> auto* {
    return "text";
}

struct Convertible {
    operator auto() {
        return 42;
    }
};
