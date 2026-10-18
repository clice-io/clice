#include "extra.h"

struct Local {
    int l;
};

#include "widget.h"

int run() {
    return make_widget().size + widget_area(make_widget()) + Local{}.l;
}
