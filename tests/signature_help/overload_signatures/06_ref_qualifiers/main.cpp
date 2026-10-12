struct Text {
    const char* data() const&;
    const char* data() &&;
    int size() const;
};

Text make();

struct Grid {
    int& at(int row, int column);
    const int& at(int row, int column) const;

    int sum() const {
        return at(0, §(implicit_this)1);
    }
};

int main() {
    Text text;
    text.data(§(lvalue));
    make().data(§(rvalue));
    text.size(§(qualified));
}
