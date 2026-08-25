/// # Switch Source/Header
///
/// ## Switch between a source file and its header
///
/// - status: unsupported
/// - order: 1
///
/// From `widget.cpp` a single command should jump to `widget.h` and
/// back — the `textDocument/switchSourceHeader` request clangd clients
/// rely on is not implemented.

// widget.h
class Widget {
    void draw();
};

// widget.cpp — #include "widget.h"
void Widget::draw() {}
