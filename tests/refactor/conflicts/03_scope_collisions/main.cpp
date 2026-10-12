#define LIMIT 10

namespace alpha {
int one = 1;
}

namespace beta {
int two = 2;
}

struct Base {
    int size;
    void grow(int amount);
};

struct Derived : Base {
    int capacity;
};

void Base::grow(int amount) { size += amount; }

int area(int width);
int volume(int width) { return width; }
int area(int width) { return volume(width); }

double scaled(double length, int factor) {
    double result = length * factor;
    return result;
}

int main() {
    Derived d;
    d.grow(LIMIT);
    return alpha::one + beta::two + d.capacity + area(2) + static_cast<int>(scaled(1.0, 2));
}
