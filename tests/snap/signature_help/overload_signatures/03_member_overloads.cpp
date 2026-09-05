/// # Member function overloads
///
/// - status: supported
///
/// A non-const receiver lists both the const and non-const overloads; the
/// trailing const qualifier is not rendered in the label

struct Buffer {
    int at(int index);
    int at(int index) const;
};

int main() {
    Buffer b;
    b.at(§(pos)0);
}
