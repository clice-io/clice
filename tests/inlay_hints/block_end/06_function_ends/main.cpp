int answer() {
    return 41;
}

template <int X>
int constant() {
    auto f = []() {
        return X;
    };
    return f();
}

int declared();

struct Pair {};
bool operator==(Pair, Pair) {
    return true;
}

struct Test {
    Test() = default;

    ~Test() {
    }

    void method1() {
    }

    void method2();

    template <typename T>
    void method3() {
    }

    template <typename T>
    void method4();

    Test operator+(int) const {
        return *this;
    }

    operator bool() const {
        return true;
    }

    operator int() const = delete;
} test;

void Test::method2() {
}

template <typename T>
void Test::method4() {
}
