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

Widget Factory::build(§(out_of_line)) {
    return Widget(800, 1.5);
}

template <typename T>
Widget convert(§(function_template));

void use() {
    Widget local(§(function_body));
}
