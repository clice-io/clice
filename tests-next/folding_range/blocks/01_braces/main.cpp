namespace store {

struct Item {
    int id;
    int count;
};

int total(const Item& item) {
    auto twice = [](int value) {
        return value * 2;
    };
    return twice(item.count);
}

}  // namespace store
