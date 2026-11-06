struct Widget {
    Widget(int width, double scale);
};

Widget make(§(namespace_scope));

Widget global(§(written_after)800, 1.5);

Widget second(800, §(after_argument)1.5);

Widget wrapped(
    §(next_line)800,
    1.5);

struct Factory {
    Widget build(§(class_scope));
};

void use() {
    Widget local(§(function_body));
}
