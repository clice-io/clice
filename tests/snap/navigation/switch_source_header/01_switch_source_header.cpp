/// # Source-header switching
///
/// - status: unsupported
///
/// From `widget.cpp` a single command should jump to `widget.h` and
/// back — the `textDocument/switchSourceHeader` request clangd clients
/// rely on is not implemented

// widget.h
class Widget {
    void draw();
};

// widget.cpp — #include "widget.h"
void Widget::draw() {}
