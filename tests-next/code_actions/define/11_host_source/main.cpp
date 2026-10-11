#include "widget.h"

namespace ns {
void Widget::done() {
}
}

int main() {
    ns::Widget w(1);
    return w.id();
}
