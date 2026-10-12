struct §(type)Widget {};

Widget make_widget();

int probe(Widget §(param)param) {
    Widget §(local)local = make_widget();
    return 0;
}
