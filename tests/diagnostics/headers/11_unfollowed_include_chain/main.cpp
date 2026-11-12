struct Widget {
    int size;
};

#define WIDGET_USER "widget_user.h"
#line 40
#include WIDGET_USER

int main() {
    return width(Widget{3});
}
