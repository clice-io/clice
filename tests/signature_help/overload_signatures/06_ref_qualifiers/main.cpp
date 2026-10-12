struct Text {
    const char* data() const&;
    const char* data() &&;
    int size() const;
};

Text make();

template <typename T>
struct Vector {
    T& at(int index);
    const T& at(int index) const;
    template <typename U>
    U at(U index, bool checked);
};

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
    Vector<int> values;
    values.at(§(class_template)0);
}
