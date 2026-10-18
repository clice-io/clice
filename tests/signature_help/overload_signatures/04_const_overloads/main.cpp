struct Buffer {
    int at(int index);
    int at(int index) const;
};

struct Pick {
    int operator()() const;
    float operator()();
};

int main() {
    Buffer b;
    b.at(§(pos)0);
    const Buffer& view = b;
    view.at(§(const_object)0);
    Pick pick;
    pick(§(call_operator));
}
