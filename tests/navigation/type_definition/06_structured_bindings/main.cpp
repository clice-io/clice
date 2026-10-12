struct §(type)Widget {};

struct Pair {
    Widget first;
    int second;
};

Pair make_pair();

int use() {
    auto [§(bound)widget, count] = make_pair();
    return 0;
}
