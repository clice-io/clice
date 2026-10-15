// Under an MSVC target a member pointer gives the record an implicit
// inheritance model; the record's and the method's cards stay as elsewhere.

namespace ms {

struct Wid§(struct_hover)get {
    int value;
    void up§(method_hover)date();
};

int Widget::* member = &Widget::value;

}
