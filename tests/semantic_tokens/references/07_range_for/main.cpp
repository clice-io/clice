// Range-for variables keep variable tokens at definitions and uses.
struct List {
    int* begin();
    int* end();
};

void iterate(List items) {
    for (auto& §(item_def)item : §(items)items) {
        §(item_use)item = 0;
    }
}
