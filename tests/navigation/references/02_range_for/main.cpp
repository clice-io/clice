struct Iterator {
    int operator*() const;
    Iterator& operator++();
    bool operator!=(const Iterator& other) const;
};

struct Range {
    Iterator §(begin)begin();
    Iterator end();
};

void use(Range r) {
    for (int x : r) {
    }
}
