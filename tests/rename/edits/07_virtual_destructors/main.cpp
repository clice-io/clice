// A class whose destructor a derived class overrides: the derived
// destructor never names the renamed class.
struct §(base)Shape {
    virtual ~Shape() {}
};

struct Circle : Shape {
    ~Circle() override {}
};

int main() {
    Circle c;
    Shape& s = c;
    return sizeof(s) > 0 ? 0 : 1;
}
