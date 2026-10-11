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
