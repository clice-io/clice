module;
#include "easel.h"
module canvas;

int easel_height() {
    return canvas_width() / easel_ratio;
}
