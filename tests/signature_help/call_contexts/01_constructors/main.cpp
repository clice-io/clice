struct Point {
    int x;
    int y = 0;
};

struct Widget {
    Widget(int a, double b);
};

int main() {
    Point p{1, §(aggregate)2};
    Widget w(§(ctor)3, 4.0);
    Widget braced{§(braced)3, 4.0};
}
