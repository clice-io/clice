struct Buffer {
    int at(int index);
    int at(int index) const;
};

int main() {
    Buffer b;
    b.at(§(pos)0);
    const Buffer& view = b;
    view.at(§(const_object)0);
}
