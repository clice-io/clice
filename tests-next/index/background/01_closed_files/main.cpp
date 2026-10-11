struct §(animal)Animal {
    virtual void §(speak)speak() {}

    virtual ~Animal() = default;
};

struct §(dog)Dog : public Animal {
    void speak() override {}
};

struct Cat : public Animal {
    void speak() override {}
};

int §(add)add(int a, int b) {
    return a + b;
}

int §(compute)compute() {
    int x = §(add_call)add(1, 2);
    int y = add(3, 4);
    return x + y;
}

int §(global_var)global_var = 42;

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
