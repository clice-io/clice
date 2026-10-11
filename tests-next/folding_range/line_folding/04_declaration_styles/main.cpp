struct Base {};
int process(int a)
{
    return a;
}
static int
gnu_style(int a,
          int b,
          int c)
{
    return a + b + c;
}
class Widget
    : public Base
{
    int x;
};
namespace
{
int y;
}
#ifdef FLAG
void picked(int a)
#else
void picked(int a, int b)
#endif
{
    return;
}
int same_line(int a) {
    return a;
}
void empty()
{
}
extern "C"
{
void c_function();
}
struct Holder
{
    int value;
    Holder()
        : value(0)
    {
        value += 1;
    }
};
