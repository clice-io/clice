/// # Special Call Contexts
///
/// ## Constructors and aggregates — constructor calls render without a return arrow; aggregate initialization lists the fields in braces
///
/// - status: supported
/// - issues: clangd#726, clangd#2541
/// - order: 1

struct Point {
    int x;
    int y;
};

struct Widget {
    Widget(int a, double b);
};

int main() {
    Point p{1, §(aggregate)2};
    Widget w(§(ctor)3, 4.0);
}
