#define MINUS_ONE - 1
#define ADDRESS(x) & x
#define SHOW(text) show(text)

int f(int* p);
void show(const char* text);

int g(int x) {
    int y = x §(binary)MINUS_ONE;
    §(raw)SHOW(R"(first
   second)");
    return f(§(unary)ADDRESS(y)) + y;
}
