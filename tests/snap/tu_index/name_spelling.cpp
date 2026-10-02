// - verify: inspect
//
// A name a macro spells part of keeps its first token alone; a
// global-qualified allocation reaches its function at the keyword.

#define NAME Widget

struct Widget {
    ~NAME();
};

void make() {
    Widget* widget = ::new Widget;
    ::delete widget;
}
