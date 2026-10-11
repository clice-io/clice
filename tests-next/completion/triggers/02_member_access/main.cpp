// `.`, `->` and `::` complete members on the trigger path too, and an
// identifier named `import` opening a line is no import statement.
struct Widget {
    int member;
    static int count;
};

void touch(Widget* import, Widget w) {
import->§(arrow)member = 1;
    w.§(dot)member = 2;
    Widget::§(scope)count = 3;
}
