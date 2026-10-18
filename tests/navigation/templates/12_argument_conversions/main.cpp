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

struct Any {
    template <typename U>
    operator U() const;
};

struct Canvas {
    void draw(this Canvas& self, int x, int y);
    void draw(this Canvas& self, Color color, int y);
};

void tag(Name name, int value);
void tag(int number, int value);

void find(Id id, int value);
void find(long number, int value);

void bump(int& counter, int value);
void bump(long counter, int value);

void take(Box&& box, int value);
void take(const Box& box, int value);

void paint(Color color, int value);
void paint(int number, int value);

void visit(Base* base, int value);
void visit(int* number, int value);

void store(Box box, int value);
void store(int number, int value);

void use(Base* base, int value);
void use(int number, int value);

void write(char* text, int value);
void write(int number, int value);

template <typename T>
void calls(T value, int count, Derived& derived, Box& box, Handle handle, Any any, Canvas& canvas) {
    §(constructor)tag("text", value);
    §(explicit_constructor)find(1, value);
    §(rvalue_to_reference)bump(1, value);
    §(lvalue_to_reference)bump(count, value);
    §(lvalue_to_rvalue_reference)take(box, value);
    §(scoped_enum)paint(Color::red, value);
    §(integer_to_enum)paint(1, value);
    §(derived_pointer)visit(&derived, value);
    §(null_pointer)visit(nullptr, value);
    §(zero)visit(0, value);
    §(conversion_function)store(handle, value);
    §(conversion_template)use(any, value);
    §(string_literal)write("text", value);
    canvas.§(explicit_object)draw(1, value);
}
