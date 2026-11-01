#pragma once

int color_red(void);

static int color_round(int x) {
    return x;
}

struct Swatch {
    int tone;
};

enum Shade { shade_light };

#define COLOR_MAX 255
