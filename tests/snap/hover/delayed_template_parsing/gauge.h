#pragma once

template <typename T>
struct Gauge {
    T level;
    T getLevel() { return level; }
};
