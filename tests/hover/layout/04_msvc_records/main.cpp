// Under an MSVC target a member pointer gives the record an implicit
// inheritance model; the record's and the method's cards stay as elsewhere.
// A property, of an unnamed struct type here, is left out of the member
// summary as the accessor it stands for is.

namespace ms {

struct Wid§(struct_hover)get {
    int value;
    void up§(method_hover)date();
};

int Widget::* member = &Widget::value;

struct Gau§(property_struct)ge {
    int level;
    int get() const;
    __declspec(property(get = get)) struct { int x; } property;
};

}
