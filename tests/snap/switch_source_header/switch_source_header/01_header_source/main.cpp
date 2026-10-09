/// # Header and its source
///
/// - status: supported
/// - verify: server
/// - indexing: true
///
/// A header and the source of the same name that defines what it declares switch to each other directly

// switch: widget.h
// switch: widget.cpp

#include "widget.h"

int render(Widget& widget) {
    widget.draw();
    return widget.area();
}
