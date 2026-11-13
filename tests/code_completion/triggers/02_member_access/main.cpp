// `.`, `->` and `::` complete members on the trigger path too, after a name
// ending in a digit or written past ASCII and after a call; an identifier
// named `import` or `module` opening a line is no import statement or module
// declaration.
struct Widget {
    int member;
    static int count;
};

Widget make(int seed);

void touch(Widget* import, Widget w) {
import->§(arrow)member = 1;
    w.§(dot)member = 2;
    Widget::§(scope)count = 3;
}

void more(Widget x1, Widget vé2, Widget module) {
    x1.§(digit_name)member = 4;
    vé2.§(non_ascii_name)member = 5;
    int got = make(1).§(call_result)member;
module.§(module_name)member = got;
}
