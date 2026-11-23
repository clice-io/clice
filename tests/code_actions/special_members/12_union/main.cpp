// A union's members are declared as a class's: a variant member with a
// non-trivial copy constructor deletes the union's.

struct Text {
    Text(const Text& other);
};

union §(value)Value {
    int integer;
    float real;
};

union §(variant)Variant {
    Variant(int number) : number(number) {}

    int number;
    Text text;
};

void use(Value& value, Variant& variant) {
    Value copy = value;
    copy = value;
    Variant other(1);
    other = variant;
    other = static_cast<Variant&&>(variant);
}
