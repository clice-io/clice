enum class Color { red };

struct Base {};
struct Derived : Base {};
struct Box {};
struct Later;

struct Any {
    template <typename U>
    operator U() const;
};

struct Text {
    template <typename S>
    Text(S source);
};

struct Point {
    int x;
    int y;
};

void visit(Base* base, int value);
void visit(int* number, int value);

void use(Base* base, int value);
void use(int number, int value);

void write(char* text, int value);
void write(int number, int value);

void enable(bool flag, int value);
void enable(Box box, int value);

void release(void* raw, int value);
void release(int number, int value);

void print_text(Text text, int value);
void print_text(int number, int value);

void send(const Later& later, int value);
void send(Base* base, int value);

void post(int number, int value);
void post(Box box, int value);

void place(Point point, int value);
void place(int number, int value);

void fill(const int (&values)[3], int value);
void fill(int number, int value);

void emit(Color color, int code = 0);
void emit(int code, Color color);

template <typename T>
void calls(T value, Derived& derived, Later& later, Any any, int (&numbers)[3]) {
    §(null_pointer)visit(nullptr, value);
    §(zero)visit(0, value);
    §(conversion_template)use(any, value);
    §(string_literal)write("text", value);
    §(pointer_to_bool)enable(&derived, value);
    §(pointer_to_void)release(&derived, value);
    §(constructor_template)print_text(&derived, value);
    §(incomplete_parameter)send(1, value);
    §(incomplete_argument)post(later, value);
    §(braced_list)place({1, 2}, value);
    §(array_rvalue)fill(static_cast<int (&&)[3]>(numbers), value);
}

template <typename... Ts>
void relay(Ts... codes) {
    §(pack_before)emit(codes..., Color::red);
}
