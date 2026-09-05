/// # Source-header switching
///
/// - status: unsupported
///
/// Source/header switching is not implemented, so users cannot jump directly
/// between paired files

// widget.h
class Widget {
    void draw();
};

// widget.cpp — #include "widget.h"
void Widget::draw() {}
