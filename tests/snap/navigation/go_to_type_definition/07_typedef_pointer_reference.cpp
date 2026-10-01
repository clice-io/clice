/// # Pointers, references and arrays
///
/// - status: supported
/// - verify: server
///
/// Go-to-type-definition looks through pointers, references and arrays to the
/// definition of the element type

struct §(type)Widget {
    int value;
};

using §(pointer_alias)WidgetPointer = Widget*;

struct Holder {
    Widget* §(field_pointer)pointer;
    Widget& §(field_reference)reference;
};

int probe(const Widget& §(const_reference)ref, Widget* §(pointer)ptr, Widget&& §(rvalue)moved,
          Widget §(array_parameter)batch[]) {
    Widget §(array)items[2];
    const Widget* §(pointer_to_const)view = ptr;
    return §(use_reference)ref.value + §(use_pointer)ptr->value + §(use_array)items[0].value +
           moved.value + view->value + batch[0].value;
}
