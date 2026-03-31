// Base class for type hierarchy
struct Animal {
    virtual void speak() {}

    virtual ~Animal() = default;
};

// Derived class for type hierarchy
struct Dog : public Animal {
    void speak() override {}
};

// Another derived class
struct Cat : public Animal {
    void speak() override {}
};

// Free function for call hierarchy
int add(int a, int b) {
    return a + b;
}

// Caller function
int compute() {
    int x = add(1, 2);
    int y = add(3, 4);
    return x + y;
}

// For find references
int global_var = 42;

int use_global() {
    return global_var + 1;
}

int use_global_again() {
    return global_var * 2;
}

int main() {
    Dog d;
    d.speak();
    Cat c;
    c.speak();
    return compute() + use_global() + use_global_again();
}
