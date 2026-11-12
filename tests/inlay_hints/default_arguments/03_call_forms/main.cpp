int fallback(int A = 4) { return A; }
int combine(int A, int B = 1, bool C = fallback()) { return A; }
int combined = combine(2);

void recurse(int = 5) { if (false) recurse(); }

struct Shape {
    Shape(float area = 3
                       + 2);
};

struct Holder {
    Holder(int, Shape shape = //
                Shape{}

           //
    ) {}
};

void build() {
    Holder first(1);
    Holder second{2};
    Holder third = {3};
    auto fourth = Holder{4};
}

#define DECLARE(name) void name(int first, int second = 4 + 1)
DECLARE(declared);
void use() { declared(1); }
