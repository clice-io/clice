#pragma once

struct Widget {
    int size;
    §(host)Host host;
    geo::§(point)Point origin;
};

inline Widget make_widget() {
    return Widget{geo::§(scale)scale(2), Host{1}, {0, 0}};
}

inline int widget_area(const Widget& widget) {
    return widget.origin.§(x)x * widget.§(size)size + geo::scale(1);
}
