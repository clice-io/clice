auto answer() {
    return 42;
}

auto& ref_answer() {
    static int storage = 0;
    return storage;
}

auto declared(int x);
auto deducible(int x);

auto deducible(int x) {
    return x + 1;
}

auto spelled() -> int;
auto pointer() -> auto* {
    return "text";
}

struct Convertible {
    operator auto() {
        return 42;
    }
};
