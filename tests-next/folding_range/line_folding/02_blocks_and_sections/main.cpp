int f(int a) {
    if (a) {
        a += 1;
    } else {
        a -= 1;
    }
    try {
        a *= 2;
    } catch (...) {
        a = 0;
    }
    do {
        a -= 1;
    } while (a > 10);
    return a;
}
class Widget {
public:
    void draw();
private:
    int width;
};
#ifdef FLAG
int x;
#else
int y;
#endif
int g(int a,
      int b);
