#pragma once

int compute(int x);

struct Widget {
    Widget();
    int value;
};

#define CALL compute(1)
