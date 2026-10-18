enum class Color { red };

struct Base {};
struct Derived : Base {};
struct Box {};

struct Name {
    Name(const char* text);
};

struct Id {
    explicit Id(int value);
};

struct Handle {
    operator Box() const;
};

struct Flag {
    explicit operator bool() const;
};

struct Canvas {
    void draw(this Canvas& self, int x, int y);
    void draw(this Canvas& self, Color color, int y);
};

void tag(Name name, int value);
void tag(int number, int value);

void find(Id id, int value);
void find(long number, int value);

void label(Name name, int value);
void label(Base* base, int value);

void rename(Name& name, int value);
void rename(const char* text, int value);

void bump(int& counter, int value);
void bump(long counter, int value);

void consume(int&& number, int value);
void consume(long number, int value);

void show(const int& number, int value);
void show(Base* base, int value);

void take(Box&& box, int value);
void take(const Box& box, int value);

void hold(Box& box, int value);
void hold(const Box& box, int value);

void keep(Base base, int value);
void keep(Box box, int value);

void paint(Color color, int value);
void paint(int number, int value);

void toggle(bool flag, int value);
void toggle(Color color, int value);

void visit(Base* base, int value);
void visit(int* number, int value);

void open_box(Base* base, int value);
void open_box(Box* box, int value);

void scan(char* text, int value);
void scan(const char* text, int value);

void load(void* raw, int value);
void load(int* number, int value);

void measure(Base* base, int value);
void measure(int number, int value);

void store(Box box, int value);
void store(int number, int value);

void check(bool flag, int value);
void check(Flag flag, int value);

template <typename T>
void calls(T value,
           int count,
           long wide,
           const int fixed_count,
           const char* view,
           void* raw,
           Derived& derived,
           Box& box,
           const Box& fixed,
           Handle handle,
           Flag flag,
           Canvas& canvas) {
    §(constructor)tag("text", value);
    §(explicit_constructor)find(1, value);
    §(constructor_parameter)label(&derived, value);
    §(constructor_to_reference)rename("text", value);
    §(rvalue_to_reference)bump(1, value);
    §(lvalue_to_reference)bump(count, value);
    §(other_lvalue_to_reference)bump(wide, value);
    §(const_lvalue_to_reference)bump(fixed_count, value);
    §(lvalue_to_rvalue_reference)consume(count, value);
    §(pointer_to_const_reference)show(&derived, value);
    §(class_lvalue_to_rvalue_reference)take(box, value);
    §(class_rvalue_to_reference)hold(Box(), value);
    §(const_class_to_reference)hold(fixed, value);
    §(derived_to_base)keep(derived, value);
    §(scoped_enum)paint(Color::red, value);
    §(integer_to_enum)paint(1, value);
    §(scoped_enum_to_bool)toggle(Color::red, value);
    §(derived_pointer)visit(&derived, value);
    §(unrelated_pointer)open_box(&box, value);
    §(lost_qualifier)scan(view, value);
    §(void_pointer)load(raw, value);
    §(null_to_integer)measure(nullptr, value);
    §(conversion_function)store(handle, value);
    §(explicit_conversion)check(flag, value);
    canvas.§(explicit_object)draw(1, value);
}
