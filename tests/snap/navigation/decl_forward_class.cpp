/// # Go to Declaration
///
/// ## Forward declarations of classes and structs
///
/// - status: supported
/// - verify: server
/// - order: 3
///
/// A class with a forward declaration and a later definition offers both
/// from a use — the forward declaration stays part of the declaration set
/// rather than being dropped in favour of the definition.

struct §(fwd)Widget;

struct §(def)Widget {
    int value;
};

class Panel;

class Panel {
    int width;
};

int probe(§(use)Widget& widget, §(class_use)Panel& panel) {
    return widget.value;
}
