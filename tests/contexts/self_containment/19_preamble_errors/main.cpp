struct Gauge {
    int level;
};

#include "panel.h"

int main() {
    return gauge_level(Gauge{1}) + panel_width();
}
