struct Text {
    const char* data() const&;
    const char* data() &&;
    int size() const;
};

Text make();

int main() {
    Text text;
    text.data(§(lvalue));
    make().data(§(rvalue));
    text.size(§(qualified));
}
